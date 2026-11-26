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
#else
#include <unistd.h>
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
      "  shift-launcher [--portable] [--game <dir>] [--tab play|setup|saves|content|mods|radio|controls|updates|android]\n"
      "  shift-launcher --screenshot <out.png> [--game <dir>] [--tab <name>] [--width 1280] [--height 720] [--scale 1.0] [--demo]\n");
}

class LauncherUiAudio {
 public:
  void init() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = 44100;
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) return;
    SDL_ResumeAudioStreamDevice(stream_);
  }

  void shutdown() {
    if (stream_) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
  }

  void play_click(bool enabled) {
    if (!enabled || !stream_) return;
    push_tone(880.0f, 0.022f, 0.07f);
  }

  void play_confirm(bool enabled) {
    if (!enabled || !stream_) return;
    push_tone(660.0f, 0.024f, 0.075f);
    push_tone(990.0f, 0.032f, 0.075f);
  }

 private:
  void push_tone(float freq_hz, float duration_s, float gain) {
    constexpr int kRate = 44100;
    const int n = std::max(1, int(duration_s * float(kRate)));
    std::vector<float> pcm(static_cast<size_t>(n));
    constexpr float kTwoPi = 6.28318530718f;
    for (int i = 0; i < n; ++i) {
      const float t = float(i) / float(kRate);
      const float env = std::exp(-t * 55.0f) * std::min(1.0f, float(i) / 48.0f);
      pcm[size_t(i)] = std::sin(kTwoPi * freq_hz * t) * gain * env;
    }
    SDL_PutAudioStreamData(stream_, pcm.data(), int(pcm.size() * sizeof(float)));
  }

  SDL_AudioStream* stream_ = nullptr;
};

void open_folder_in_shell(const std::filesystem::path& dir, std::string& notice) {
  if (dir.empty()) {
    notice = "Play folder path is not set.";
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", dir.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  notice = "Opened " + dir.string();
#elif defined(__APPLE__)
  std::string err;
  if (launcher::launch_detached_process({"open", dir.string()}, {}, &err)) {
    notice = "Opened " + dir.string();
  } else {
    notice = "Failed to open folder: " + err;
  }
#else
  std::string err;
  if (launcher::launch_detached_process({"xdg-open", dir.string()}, {}, &err)) {
    notice = "Opened " + dir.string();
  } else {
    notice = "Failed to open folder: " + err;
  }
#endif
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

enum class DialogKind {
  kBrowseIso,
  kBrowseFolder,
  kBrowseStateRoot,
  kBrowseSaveFile,
  kBrowseSaveFolder,
  kBrowsePinyonFolder,
  kBrowsePinyonZip,
  kImportStfs,
  kImportContentFolder,
  kInstallModZip,
  kInstallModFolder,
  kBrowseRadioMusic,
  kBrowseRadioLogo,
};

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

void load_synced_settings(launcher::LauncherUiState& st) {
  st.wheel_cfg = launcher::load_wheel_settings(st.paths.settings_file);
  st.selected_preset = launcher::preset_index_for_name(launcher::load_graphics_preset_setting(st.paths.settings_file));
  st.selected_resolution = launcher::load_resolution_setting(st.paths.settings_file);
  st.launch_fullscreen = launcher::load_fullscreen_setting(st.paths.settings_file);
  st.selected_vsync = launcher::vsync_index_for_name(launcher::load_vsync_setting(st.paths.settings_file));
  st.selected_fps_cap = launcher::fps_cap_index_for_name(launcher::load_fps_cap_setting(st.paths.settings_file));
  st.launcher_sounds_enabled = launcher::load_launcher_sounds_setting(st.paths.settings_file);
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
  st.save_profiles = launcher::scan_save_profiles(st.paths);
  if (st.selected_save_profile >= int(st.save_profiles.size())) {
    st.selected_save_profile = std::max(0, int(st.save_profiles.size()) - 1);
  }
}

void refresh_all(launcher::LauncherUiState& st, const std::vector<std::filesystem::path>& extra_pinyon_roots = {}) {
  launcher::read_play_config(st.paths.play_dir, st.play_cfg);
  if (!st.play_cfg.game_root.empty() && st.disc_path_input.empty()) {
    st.disc_path_input = st.play_cfg.game_root.string();
  }
  if (!st.play_cfg.state_root.empty()) {
    st.paths.state_dir = st.play_cfg.state_root;
  }
  if (st.state_path_input.empty() && !st.paths.state_dir.empty()) {
    st.state_path_input = st.paths.state_dir.string();
  }
  st.disc_check = launcher::verify_disc_folder(st.disc_path_input, st.paths.play_dir);
  st.content_status = launcher::scan_content_packages(st.paths.state_dir, st.paths.mods_dir, st.paths.play_dir);
  st.mods = launcher::scan_mods(st.paths.mods_dir);
  st.save_iso = launcher::evaluate_save_isolation(st.mods);
  if (st.selected_mod >= int(st.mods.size())) st.selected_mod = std::max(0, int(st.mods.size()) - 1);
  load_synced_settings(st);
  st.pinyon_sources = launcher::scan_pinyon_save_sources(extra_pinyon_roots);
  if (st.selected_pinyon_source >= int(st.pinyon_sources.size())) {
    st.selected_pinyon_source = std::max(0, int(st.pinyon_sources.size()) - 1);
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
  const std::string preset_arg = launcher::preset_name_for_index(st.selected_preset);
  std::vector<std::string> args = {st.paths.carview_bin.string(), "--drive"};
  if (!st.paths.play_dir.empty()) {
    args.push_back("--play");
    args.push_back(st.paths.play_dir.string());
  }
  args.push_back("--preset");
  args.push_back(preset_arg);
  const auto& res = launcher::kResolutionPresets[std::clamp(st.selected_resolution, 0, launcher::kResolutionPresetCount - 1)];
  if (res.width > 0 && res.height > 0) {
    args.push_back("--size");
    args.push_back(std::to_string(res.width) + "x" + std::to_string(res.height));
  }
  args.push_back(st.launch_fullscreen ? "--fullscreen" : "--no-fullscreen");
  args.push_back("--vsync");
  args.push_back(launcher::vsync_name_for_index(st.selected_vsync));
  args.push_back("--fps-cap");
  args.push_back(launcher::fps_cap_name_for_index(st.selected_fps_cap));
  {
    const auto backends = launcher::available_backend_choices();
    if (!backends.empty()) {
      const auto& choice = backends[size_t(std::clamp(st.selected_backend, 0, int(backends.size()) - 1))];
      if (std::strcmp(choice.id, "auto") != 0) {
        args.push_back("--backend");
        args.push_back(choice.id);
      }
    }
  }
  {
    std::string active_profile;
    for (const auto& p : st.save_profiles) {
      if (p.active) {
        active_profile = p.name;
        break;
      }
    }
    if (active_profile.empty()) active_profile = launcher::load_profile_setting(st.paths.settings_file);
    if (!active_profile.empty()) {
      args.push_back("--profile");
      args.push_back(active_profile);
    }
  }
  if (!st.save_import_path.empty()) {
    std::error_code ec;
    const std::filesystem::path sp(st.save_import_path);
    if (std::filesystem::is_directory(sp, ec)) {
      args.push_back("--import-profile");
      args.push_back(sp.string());
    } else if (std::filesystem::is_regular_file(sp, ec)) {
      args.push_back("--save");
      args.push_back(sp.string());
    }
  }
  if (!st.launch_car.empty()) {
    args.push_back("--car");
    args.push_back(st.launch_car);
  }
  if (!st.extra_args.empty()) {
    const auto extra = launcher::split_command_args(st.extra_args);
    args.insert(args.end(), extra.begin(), extra.end());
  }

  std::vector<std::pair<std::string, std::string>> env;
  if (!st.paths.settings_file.empty()) {
    env.emplace_back("SHIFT_SETTINGS", st.paths.settings_file.string());
  }
  const std::filesystem::path effective_state =
      !st.state_path_input.empty() ? std::filesystem::path(st.state_path_input) : st.paths.state_dir;
  if (!effective_state.empty()) {
    env.emplace_back("SHIFT_STATE_ROOT", effective_state.string());
  }

  std::string err;
  if (!launcher::launch_detached_process(args, env, &err)) {
    notice = err.empty() ? "Failed to launch carview." : err;
    return false;
  }
  notice = "Launched carview (" + preset_arg + " preset).";
  return true;
}

void scroll_active_tab(launcher::LauncherUiState& st, int delta) {
  if (delta == 0) return;
  switch (st.active_tab) {
    case launcher::LauncherTab::kSetup:
      st.setup_log_scroll = std::max(0, st.setup_log_scroll + delta);
      break;
    case launcher::LauncherTab::kSaves:
      st.save_profiles_scroll = std::max(0, st.save_profiles_scroll - delta);
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

  std::filesystem::path exe_path;
  std::filesystem::path exe_dir;
  if (argc > 0 && argv[0]) {
    std::error_code ec;
    exe_path = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec);
    if (exe_path.empty()) exe_path = std::filesystem::path(argv[0]);
    exe_dir = exe_path.parent_path();
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

  LauncherUiAudio ui_audio;
  ui_audio.init();

  launcher::SetupRunner setup_runner;
  launcher::ContentInstallRunner content_runner;
  launcher::WifiSyncServer wifi_server;
  DialogQueue dialog_queue;
  std::vector<std::filesystem::path> extra_pinyon_roots;
  bool was_setup_running = false;
  bool was_content_running = false;
  bool text_input_active = false;
  bool quit_for_update = false;
  carview::ui::List ui_list;
  std::vector<launcher::HitBox> hitboxes;

  // Lightweight file-mtime tracking for bidirectional settings.txt & profiles.txt reload on focus gain.
  std::filesystem::file_time_type last_settings_mtime{};
  std::filesystem::file_time_type last_profiles_mtime{};
  bool has_settings_mtime = false;
  bool has_profiles_mtime = false;

  auto query_mtime = [](const std::filesystem::path& p, std::filesystem::file_time_type& out) -> bool {
    if (p.empty()) return false;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    out = t;
    return true;
  };
  auto record_sync_mtimes = [&]() {
    has_settings_mtime = query_mtime(state.paths.settings_file, last_settings_mtime);
    has_profiles_mtime = query_mtime(state.paths.data_root / "profiles.txt", last_profiles_mtime);
  };
  auto sync_external_settings_if_changed = [&]() -> bool {
    std::filesystem::file_time_type cur_st{}, cur_pr{};
    const bool cur_has_st = query_mtime(state.paths.settings_file, cur_st);
    const bool cur_has_pr = query_mtime(state.paths.data_root / "profiles.txt", cur_pr);
    const bool st_changed = (cur_has_st != has_settings_mtime) || (cur_has_st && cur_st != last_settings_mtime);
    const bool pr_changed = (cur_has_pr != has_profiles_mtime) || (cur_has_pr && cur_pr != last_profiles_mtime);
    if (!st_changed && !pr_changed) return false;
    has_settings_mtime = cur_has_st;
    last_settings_mtime = cur_st;
    has_profiles_mtime = cur_has_pr;
    last_profiles_mtime = cur_pr;
    load_synced_settings(state);
    return true;
  };
  record_sync_mtimes();

  auto active_text_field = [&]() -> std::string* {
    if (state.active_tab == launcher::LauncherTab::kSetup) {
      if (state.focused_id == "setup:path") return &state.disc_path_input;
      if (state.focused_id == "setup:state_path") return &state.state_path_input;
      if (state.focused_id == "setup:save_path") return &state.save_import_path;
    } else if (state.active_tab == launcher::LauncherTab::kSaves) {
      if (state.focused_id == "saves:new_tag") return &state.new_profile_gamertag;
    }
    return nullptr;
  };

  auto sync_text_input = [&]() {
    const bool want = active_text_field() != nullptr;
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

  auto apply_state_root = [&](const std::filesystem::path& p) {
    const std::string cleaned = clean_pasted_path(p.string());
    state.state_path_input = cleaned;
    if (!cleaned.empty()) {
      state.paths.state_dir = std::filesystem::path(cleaned);
      if (state.play_cfg.game_root_valid && !state.play_cfg.game_root.empty()) {
        launcher::write_play_config(state.paths.play_dir, state.play_cfg.game_root, state.paths.state_dir);
      }
      state.content_status = launcher::scan_content_packages(state.paths.state_dir, state.paths.mods_dir, state.paths.play_dir);
      state.banner_notice = "State folder set to " + state.paths.state_dir.string();
    } else {
      state.banner_notice = "State folder cleared (will use default).";
    }
    state.active_tab = launcher::LauncherTab::kSetup;
  };

  auto apply_save_import_path = [&](const std::filesystem::path& p) {
    state.save_import_path = clean_pasted_path(p.string());
    if (state.save_import_path.empty()) {
      state.banner_notice = "Cleared save import path.";
    } else {
      std::error_code ec;
      const std::filesystem::path sp(state.save_import_path);
      if (std::filesystem::is_directory(sp, ec)) {
        state.banner_notice = "Save profile export folder selected: " + sp.filename().string();
      } else if (std::filesystem::is_regular_file(sp, ec)) {
        state.banner_notice = "Save file selected: " + sp.filename().string();
      } else {
        state.banner_notice = "Save import path set (verify path exists before Play): " + state.save_import_path;
      }
    }
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
  static constexpr SDL_DialogFileFilter kSaveFilters[2] = {
      {"Save / Profile Files", "bin;sav;dat;profile"},
      {"All Files", "*"},
  };
  static constexpr SDL_DialogFileFilter kModZipFilters[2] = {
      {"Mod ZIP Archives", "zip"},
      {"All Files", "*"},
  };
  static constexpr SDL_DialogFileFilter kPngFilters[2] = {
      {"PNG Images", "png"},
      {"All Files", "*"},
  };

  auto handle_click = [&](const std::string& id) {
    const bool is_primary_act =
        (id == "act:play" || id == "setup:run" || id == "wheel:save" || id == "radio:build" || id == "upd:apply");
    if (id != "setup:toggle_sounds") {
      if (is_primary_act) ui_audio.play_confirm(state.launcher_sounds_enabled);
      else ui_audio.play_click(state.launcher_sounds_enabled);
    }

    if (id.rfind("tab:", 0) == 0) {
      launcher::LauncherTab t;
      if (launcher::parse_tab_name(id.substr(4), t)) {
        state.active_tab = t;
        state.focused_id.clear();
      }
    } else if (id.rfind("preset:", 0) == 0) {
      state.selected_preset = std::clamp(std::atoi(id.c_str() + 7), 0, launcher::kGraphicsPresetCount - 1);
      const char* pname = launcher::preset_name_for_index(state.selected_preset);
      launcher::save_graphics_preset_setting(state.paths.settings_file, pname);
      record_sync_mtimes();
      state.banner_notice = std::string("Graphics preset set to ") +
                            launcher::kGraphicsPresets[state.selected_preset].label + " (" + pname + ").";
    } else if (id.rfind("res:", 0) == 0) {
      state.selected_resolution = std::clamp(std::atoi(id.c_str() + 4), 0, launcher::kResolutionPresetCount - 1);
      launcher::save_resolution_setting(state.paths.settings_file, state.selected_resolution);
      record_sync_mtimes();
      state.banner_notice = std::string("Resolution set to ") +
                            launcher::kResolutionPresets[state.selected_resolution].label + ".";
    } else if (id.rfind("backend:", 0) == 0) {
      const auto backends = launcher::available_backend_choices();
      if (!backends.empty()) {
        state.selected_backend = std::clamp(std::atoi(id.c_str() + 8), 0, int(backends.size()) - 1);
        const auto& choice = backends[size_t(state.selected_backend)];
        launcher::save_graphics_backend_setting(state.paths.settings_file, choice.id);
        record_sync_mtimes();
        state.banner_notice = std::string("Graphics backend set to ") + choice.label + ".";
      }
    } else if (id.rfind("vsync:", 0) == 0) {
      state.selected_vsync = std::clamp(std::atoi(id.c_str() + 6), 0, launcher::kVsyncChoiceCount - 1);
      const auto& vc = launcher::kVsyncChoices[state.selected_vsync];
      launcher::save_vsync_setting(state.paths.settings_file, vc.id);
      record_sync_mtimes();
      state.banner_notice = std::string("VSync set to ") + vc.label + ".";
    } else if (id.rfind("fps:", 0) == 0) {
      state.selected_fps_cap = std::clamp(std::atoi(id.c_str() + 4), 0, launcher::kFpsCapChoiceCount - 1);
      const auto& fc = launcher::kFpsCapChoices[state.selected_fps_cap];
      launcher::save_fps_cap_setting(state.paths.settings_file, fc.id);
      record_sync_mtimes();
      state.banner_notice = std::string("FPS cap set to ") + fc.label + ".";
    } else if (id == "act:fullscreen") {
      state.launch_fullscreen = !state.launch_fullscreen;
      launcher::save_fullscreen_setting(state.paths.settings_file, state.launch_fullscreen);
      record_sync_mtimes();
      state.banner_notice = state.launch_fullscreen ? "Window mode set to Fullscreen." : "Window mode set to Windowed.";
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
      state.state_path_input.clear();
      refresh_all(state, extra_pinyon_roots);
      record_sync_mtimes();
    } else if (id == "act:apply_rec") {
      state.selected_preset = launcher::preset_index_for_name(state.hw_rec.carview_preset);
      launcher::save_graphics_preset_setting(state.paths.settings_file,
                                             launcher::preset_name_for_index(state.selected_preset));
      record_sync_mtimes();
      state.banner_notice = "Selected recommended preset: " + state.hw_rec.label;
    } else if (id == "saves:new_tag") {
      state.focused_id = id;
    } else if (id.rfind("saves:prof:", 0) == 0) {
      const int idx = std::atoi(id.c_str() + 11);
      if (idx >= 0 && idx < int(state.save_profiles.size())) state.selected_save_profile = idx;
    } else if (id.rfind("saves:src:", 0) == 0) {
      const int idx = std::atoi(id.c_str() + 10);
      if (idx >= 0 && idx < int(state.pinyon_sources.size())) state.selected_pinyon_source = idx;
    } else if (id == "saves:set_active") {
      if (state.selected_save_profile >= 0 && size_t(state.selected_save_profile) < state.save_profiles.size()) {
        const std::string target = state.save_profiles[size_t(state.selected_save_profile)].name;
        std::string err;
        if (launcher::set_active_save_profile(state.paths, target, &err)) {
          state.save_profiles = launcher::scan_save_profiles(state.paths);
          record_sync_mtimes();
          state.saves_status = "Active profile switched to '" + target + "'.";
          state.banner_notice = state.saves_status;
        } else {
          state.saves_status = err.empty() ? "Could not switch active profile." : err;
          state.banner_notice = state.saves_status;
        }
      }
    } else if (id == "saves:new") {
      std::string tag = clean_pasted_path(state.new_profile_gamertag);
      if (tag.empty()) tag = "Driver " + std::to_string(state.save_profiles.size() + 1);
      std::string created_name, err;
      if (launcher::create_save_profile(state.paths, tag, &created_name, &err)) {
        state.new_profile_gamertag.clear();
        state.save_profiles = launcher::scan_save_profiles(state.paths);
        for (int i = 0; i < int(state.save_profiles.size()); ++i) {
          if (state.save_profiles[size_t(i)].name == created_name) state.selected_save_profile = i;
        }
        record_sync_mtimes();
        state.saves_status = "Created profile '" + created_name + "' (" + tag + ").";
        state.banner_notice = state.saves_status;
      } else {
        state.saves_status = err.empty() ? "Could not create profile." : err;
        state.banner_notice = state.saves_status;
      }
    } else if (id == "saves:duplicate") {
      if (state.selected_save_profile >= 0 && size_t(state.selected_save_profile) < state.save_profiles.size()) {
        const std::string src_name = state.save_profiles[size_t(state.selected_save_profile)].name;
        std::string dup_name, err;
        if (launcher::duplicate_save_profile(state.paths, src_name, &dup_name, &err)) {
          state.save_profiles = launcher::scan_save_profiles(state.paths);
          for (int i = 0; i < int(state.save_profiles.size()); ++i) {
            if (state.save_profiles[size_t(i)].name == dup_name) state.selected_save_profile = i;
          }
          record_sync_mtimes();
          state.saves_status = "Duplicated '" + src_name + "' -> '" + dup_name + "'.";
          state.banner_notice = state.saves_status;
        } else {
          state.saves_status = err.empty() ? "Could not duplicate profile." : err;
          state.banner_notice = state.saves_status;
        }
      }
    } else if (id == "saves:backup") {
      if (state.selected_save_profile >= 0 && size_t(state.selected_save_profile) < state.save_profiles.size()) {
        const std::string target = state.save_profiles[size_t(state.selected_save_profile)].name;
        std::string label, err;
        if (launcher::backup_save_profile(state.paths, target, &label, &err)) {
          state.save_profiles = launcher::scan_save_profiles(state.paths);
          state.saves_status = "Backed up '" + target + "' (" + label + ").";
          state.banner_notice = state.saves_status;
        } else {
          state.saves_status = err.empty() ? "Backup skipped." : err;
          state.banner_notice = state.saves_status;
        }
      }
    } else if (id == "saves:delete") {
      if (state.selected_save_profile >= 0 && size_t(state.selected_save_profile) < state.save_profiles.size()) {
        const std::string target = state.save_profiles[size_t(state.selected_save_profile)].name;
        std::string err;
        if (launcher::delete_save_profile(state.paths, target, &err)) {
          state.save_profiles = launcher::scan_save_profiles(state.paths);
          if (state.selected_save_profile >= int(state.save_profiles.size())) {
            state.selected_save_profile = std::max(0, int(state.save_profiles.size()) - 1);
          }
          record_sync_mtimes();
          state.saves_status = "Removed profile '" + target + "' (files moved aside safely).";
          state.banner_notice = state.saves_status;
        } else {
          state.saves_status = err.empty() ? "Cannot delete profile." : err;
          state.banner_notice = state.saves_status;
        }
      }
    } else if (id == "saves:open_folder") {
      if (state.selected_save_profile >= 0 && size_t(state.selected_save_profile) < state.save_profiles.size()) {
        open_folder_in_shell(state.save_profiles[size_t(state.selected_save_profile)].save_dir, state.banner_notice);
      } else {
        open_folder_in_shell(state.paths.drive_dir, state.banner_notice);
      }
    } else if (id == "saves:rescan") {
      state.pinyon_sources = launcher::scan_pinyon_save_sources(extra_pinyon_roots);
      if (state.selected_pinyon_source >= int(state.pinyon_sources.size())) {
        state.selected_pinyon_source = std::max(0, int(state.pinyon_sources.size()) - 1);
      }
      state.saves_status = "Found " + std::to_string(state.pinyon_sources.size()) + " PinyonShift save source(s).";
      state.banner_notice = state.saves_status;
    } else if (id == "saves:browse_folder") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowsePinyonFolder};
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, nullptr, false);
    } else if (id == "saves:browse_zip") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowsePinyonZip};
      SDL_ShowOpenFileDialog(on_file_dialog_complete, ctx, window, kModZipFilters, 2, nullptr, false);
    } else if (id == "saves:import_sel" || id == "saves:import_new") {
      if (state.selected_pinyon_source < 0 || size_t(state.selected_pinyon_source) >= state.pinyon_sources.size()) {
        state.saves_status = "Select a PinyonShift save source first.";
        state.banner_notice = state.saves_status;
      } else {
        const auto& src = state.pinyon_sources[size_t(state.selected_pinyon_source)];
        const bool as_new = (id == "saves:import_new");
        std::string target_name;
        if (!as_new && state.selected_save_profile >= 0 &&
            size_t(state.selected_save_profile) < state.save_profiles.size()) {
          target_name = state.save_profiles[size_t(state.selected_save_profile)].name;
        } else if (as_new) {
          target_name = clean_pasted_path(state.new_profile_gamertag);
        }
        std::string summary, err;
        if (launcher::import_pinyon_save(state.paths, src.path, target_name, as_new, !as_new, &summary, &err)) {
          state.save_profiles = launcher::scan_save_profiles(state.paths);
          record_sync_mtimes();
          state.saves_status = summary;
          state.banner_notice = summary;
        } else {
          state.saves_status = err.empty() ? "Import failed." : err;
          state.banner_notice = state.saves_status;
        }
      }
    } else if (id == "setup:path" || id == "setup:state_path" || id == "setup:save_path") {
      state.focused_id = id;
    } else if (id == "setup:browse_iso") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseIso};
      SDL_ShowOpenFileDialog(on_file_dialog_complete, ctx, window, kIsoFilters, 2, nullptr, false);
    } else if (id == "setup:browse_folder") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseFolder};
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, nullptr, false);
    } else if (id == "setup:browse_state") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseStateRoot};
      const char* def_loc = state.state_path_input.empty() ? nullptr : state.state_path_input.c_str();
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, def_loc, false);
    } else if (id == "setup:browse_save_file") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseSaveFile};
      const char* def_loc = state.save_import_path.empty() ? nullptr : state.save_import_path.c_str();
      SDL_ShowOpenFileDialog(on_file_dialog_complete, ctx, window, kSaveFilters, 2, def_loc, false);
    } else if (id == "setup:browse_save_folder") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseSaveFolder};
      const char* def_loc = state.save_import_path.empty() ? nullptr : state.save_import_path.c_str();
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, def_loc, false);
    } else if (id == "setup:verify") {
      state.disc_path_input = clean_pasted_path(state.disc_path_input);
      state.disc_check = launcher::verify_disc_folder(state.disc_path_input, state.paths.play_dir);
      state.banner_notice = state.disc_check.summary;
    } else if (id == "setup:toggle_sounds") {
      state.launcher_sounds_enabled = !state.launcher_sounds_enabled;
      state.setup_all_sounds = state.launcher_sounds_enabled;
      launcher::save_launcher_sounds_setting(state.paths.settings_file, state.launcher_sounds_enabled);
      record_sync_mtimes();
      if (state.launcher_sounds_enabled) {
        ui_audio.play_confirm(true);
        state.banner_notice = "Launcher UI sounds enabled.";
      } else {
        state.banner_notice = "Launcher UI sounds muted.";
      }
    } else if (id == "setup:toggle_index") {
      state.setup_build_index = !state.setup_build_index;
    } else if (id == "setup:toggle_log") {
      state.setup_log_expanded = !state.setup_log_expanded;
    } else if (id == "setup:cancel") {
      setup_runner.cancel();
      state.banner_notice = "Cancelling setup...";
    } else if (id == "setup:run") {
      state.disc_path_input = clean_pasted_path(state.disc_path_input);
      state.state_path_input = clean_pasted_path(state.state_path_input);
      if (!state.state_path_input.empty()) state.paths.state_dir = std::filesystem::path(state.state_path_input);
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
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kInstallModZip};
      SDL_ShowOpenFileDialog(on_file_dialog_complete, ctx, window, kModZipFilters, 2, nullptr, true);
    } else if (id == "mods:install_folder") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kInstallModFolder};
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, nullptr, true);
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
    } else if (id == "radio:browse_music") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseRadioMusic};
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, nullptr, false);
    } else if (id == "radio:browse_logo") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseRadioLogo};
      SDL_ShowOpenFileDialog(on_file_dialog_complete, ctx, window, kPngFilters, 2, nullptr, false);
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
        record_sync_mtimes();
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
      constexpr const char* kPlatformSelector = "windows";
#elif defined(__APPLE__)
      constexpr const char* kPlatformSelector = "macos";
#else
      constexpr const char* kPlatformSelector = "linux";
#endif
      const auto ur = launcher::check_github_updates(state.current_version, state.update_channel, kPlatformSelector);
      state.available_update = ur.update;
      state.staged_apply_script.clear();
      state.staged_file_count = 0;
      state.update_status = ur.status;
      state.banner_notice = ur.status;
    } else if (id == "upd:stage") {
      if (!state.available_update) {
        state.update_status = "No update selected. Check for updates first.";
        state.banner_notice = state.update_status;
      } else {
#ifdef _WIN32
        const uint32_t self_pid = uint32_t(GetCurrentProcessId());
#else
        const uint32_t self_pid = uint32_t(getpid());
#endif
        const auto sr = launcher::stage_github_update(*state.available_update, state.paths.launcher_dir, self_pid,
                                                      exe_path);
        if (sr.ok) {
          state.staged_apply_script = sr.apply_script_path;
          state.staged_file_count = int(sr.plan.moves.size());
        } else {
          state.staged_apply_script.clear();
          state.staged_file_count = 0;
        }
        state.update_status = sr.status;
        state.banner_notice = sr.status;
      }
    } else if (id == "upd:apply") {
      if (state.staged_apply_script.empty()) {
        state.update_status = "Stage and verify an update before applying.";
        state.banner_notice = state.update_status;
      } else {
        std::string aerr;
        if (launcher::launch_staged_update_script(state.staged_apply_script, &aerr)) {
          state.update_status = "Applying staged update and restarting launcher...";
          state.banner_notice = state.update_status;
          quit_for_update = true;
        } else {
          state.update_status = "Could not start staged update: " + aerr;
          state.banner_notice = state.update_status;
        }
      }
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

  auto add_pinyon_source_path = [&](const std::filesystem::path& p) {
    extra_pinyon_roots.push_back(p);
    state.pinyon_sources = launcher::scan_pinyon_save_sources(extra_pinyon_roots);
    std::error_code ec;
    const auto want = std::filesystem::weakly_canonical(p, ec);
    for (int i = 0; i < int(state.pinyon_sources.size()); ++i) {
      const auto cand = std::filesystem::weakly_canonical(state.pinyon_sources[size_t(i)].path, ec);
      if (cand == want || state.pinyon_sources[size_t(i)].path == p) {
        state.selected_pinyon_source = i;
        break;
      }
    }
    state.active_tab = launcher::LauncherTab::kSaves;
    if (state.selected_pinyon_source >= 0 && size_t(state.selected_pinyon_source) < state.pinyon_sources.size()) {
      const auto& s = state.pinyon_sources[size_t(state.selected_pinyon_source)];
      state.saves_status = s.importable ? ("Ready to import: " + s.label) : s.detail;
    } else {
      state.saves_status = "No PinyonShift save export found at " + p.filename().string();
    }
    state.banner_notice = state.saves_status;
  };

  float user_zoom = 1.0f;
  bool running = true;
  bool window_focused = true;
  bool window_minimized = false;
  bool window_occluded = false;
  bool dirty = true;
  bool atlas_synced = false;
  size_t last_setup_logs = 0;
  float last_setup_frac = -1.0f;
  std::string last_setup_stage;
  float last_content_frac = -1.0f;
  std::string last_content_stage;
  size_t last_sync_activity_count = 0;
  float last_live_steer = 0.0f;
  float last_live_throttle = 0.0f;
  float last_live_brake = 0.0f;

  auto process_event = [&](const SDL_Event& e, float scale_x, float scale_y) {
    if (e.type == SDL_EVENT_QUIT) {
      running = false;
    } else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
      window_focused = true;
      window_minimized = false;
      window_occluded = false;
      renderer.set_window_focused(true);
      sync_external_settings_if_changed();
      dirty = true;
    } else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
      window_focused = false;
      renderer.set_window_focused(false);
      state.hovered_id.clear();
      state.pressed_id.clear();
      dirty = true;
    } else if (e.type == SDL_EVENT_WINDOW_MINIMIZED) {
      window_minimized = true;
      window_focused = false;
      renderer.set_window_focused(false);
    } else if (e.type == SDL_EVENT_WINDOW_OCCLUDED) {
      window_occluded = true;
    } else if (e.type == SDL_EVENT_WINDOW_RESTORED || e.type == SDL_EVENT_WINDOW_EXPOSED) {
      window_minimized = false;
      window_occluded = false;
      dirty = true;
    } else if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
               e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
      dirty = true;
    } else if (e.type == SDL_EVENT_TEXT_INPUT) {
      if (std::string* field = active_text_field(); field && e.text.text) {
        for (const char* p = e.text.text; *p; ++p) {
          if (*p != '\r' && *p != '\n') field->push_back(*p);
        }
        dirty = true;
      }
    } else if (e.type == SDL_EVENT_KEY_DOWN) {
      const bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
      const bool ctrl = (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
      std::string* editing_field = active_text_field();
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
          if (!pasted.empty()) {
            if (state.focused_id == "setup:state_path") apply_state_root(pasted);
            else if (state.focused_id == "setup:save_path") apply_save_import_path(pasted);
            else apply_disc_source(pasted);
          }
        }
      } else if (editing_field) {
        if (e.key.key == SDLK_ESCAPE) {
          state.focused_id.clear();
        } else if (e.key.key == SDLK_BACKSPACE) {
          if (ctrl) editing_field->clear();
          else pop_utf8_char(*editing_field);
        } else if (e.key.key == SDLK_DELETE) {
          editing_field->clear();
        } else if (e.key.key == SDLK_RETURN) {
          if (state.focused_id == "saves:new_tag") {
            handle_click("saves:new");
          } else if (state.focused_id == "setup:state_path") {
            apply_state_root(state.state_path_input);
          } else if (state.focused_id == "setup:save_path") {
            apply_save_import_path(state.save_import_path);
          } else {
            apply_disc_source(state.disc_path_input);
          }
          state.focused_id.clear();
        } else if (e.key.key == SDLK_TAB) {
          cycle_focus(state, hitboxes, shift ? -1 : 1);
        }
      } else if (e.key.key == SDLK_ESCAPE) {
        if (!state.focused_id.empty()) state.focused_id.clear();
        else running = false;
      } else if (e.key.key >= SDLK_1 && e.key.key <= SDLK_9) {
        const int idx = int(e.key.key - SDLK_1);
        if (idx < int(launcher::LauncherTab::kCount)) {
          state.active_tab = launcher::LauncherTab(idx);
          state.focused_id.clear();
        }
      } else if (e.key.key == SDLK_TAB) {
        if (ctrl) {
          const int n = int(launcher::LauncherTab::kCount);
          state.active_tab = launcher::LauncherTab((int(state.active_tab) + (shift ? n - 1 : 1)) % n);
          state.focused_id.clear();
        } else {
          cycle_focus(state, hitboxes, shift ? -1 : 1);
        }
      } else if (e.key.key == SDLK_UP) {
        if (state.active_tab == launcher::LauncherTab::kSaves && !state.save_profiles.empty()) {
          state.selected_save_profile = std::max(0, state.selected_save_profile - 1);
        } else if (state.active_tab == launcher::LauncherTab::kMods && !state.mods.empty()) {
          state.selected_mod = std::max(0, state.selected_mod - 1);
        } else if (state.active_tab == launcher::LauncherTab::kWheel && !state.devices.empty()) {
          state.selected_device = std::max(0, state.selected_device - 1);
        } else {
          const int n = int(launcher::LauncherTab::kCount);
          state.active_tab = launcher::LauncherTab((int(state.active_tab) + n - 1) % n);
        }
      } else if (e.key.key == SDLK_DOWN) {
        if (state.active_tab == launcher::LauncherTab::kSaves && !state.save_profiles.empty()) {
          state.selected_save_profile =
              std::min(int(state.save_profiles.size()) - 1, state.selected_save_profile + 1);
        } else if (state.active_tab == launcher::LauncherTab::kMods && !state.mods.empty()) {
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
      dirty = true;
    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
      state.mouse_x = e.motion.x * scale_x;
      state.mouse_y = e.motion.y * scale_y;
      std::string next_hover;
      for (const auto& hb : hitboxes) {
        if (hb.contains(state.mouse_x, state.mouse_y)) next_hover = hb.id;
      }
      if (next_hover != state.hovered_id) {
        state.hovered_id = std::move(next_hover);
        dirty = true;
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
      dirty = true;
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
      dirty = true;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
      if (e.wheel.y > 0.0f) scroll_active_tab(state, 2);
      else if (e.wheel.y < 0.0f) scroll_active_tab(state, -2);
      dirty = true;
    } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
      const std::filesystem::path dropped(e.drop.data);
      std::error_code drop_ec;
      const bool is_file = std::filesystem::is_regular_file(dropped, drop_ec);
      if (state.active_tab == launcher::LauncherTab::kSaves) {
        add_pinyon_source_path(dropped);
      } else if (is_file && launcher::is_stfs_package_file(dropped)) {
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
      dirty = true;
    } else if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED) {
      rescan_joys();
      dirty = true;
    }
  };

  while (running && !quit_for_update) {
    const auto [scale_x, scale_y] = current_pixel_scale();
    const bool backgrounded = window_minimized || window_occluded;
    const bool live_wheel_poll = (state.active_tab == launcher::LauncherTab::kWheel && !open_joys.empty());
    const bool active_anim = state.setup_progress.running || state.content_install.running || live_wheel_poll;

    SDL_Event e;
    if (dirty && !backgrounded) {
      while (SDL_PollEvent(&e)) process_event(e, scale_x, scale_y);
    } else {
      const int wait_ms = backgrounded ? 100 : (active_anim ? 16 : 100);
      if (SDL_WaitEventTimeout(&e, wait_ms)) {
        process_event(e, scale_x, scale_y);
        while (SDL_PollEvent(&e)) process_event(e, scale_x, scale_y);
      }
    }

    sync_text_input();

    // Drain native OS file/folder picker callbacks safely on the main thread
    std::vector<PendingDialogResult> dialog_results;
    {
      std::lock_guard<std::mutex> lk(dialog_queue.mu);
      dialog_results.swap(dialog_queue.items);
    }
    if (!dialog_results.empty()) dirty = true;
    for (const auto& dr : dialog_results) {
      if (!dr.error.empty()) {
        state.banner_notice = "File picker unavailable (" + dr.error + ") - drag and drop or paste path (Ctrl+V).";
        continue;
      }
      if (dr.cancelled || dr.paths.empty()) continue;
      if (dr.kind == DialogKind::kBrowseIso || dr.kind == DialogKind::kBrowseFolder) {
        apply_disc_source(dr.paths.front());
      } else if (dr.kind == DialogKind::kBrowseStateRoot) {
        apply_state_root(dr.paths.front());
      } else if (dr.kind == DialogKind::kBrowseSaveFile || dr.kind == DialogKind::kBrowseSaveFolder) {
        apply_save_import_path(dr.paths.front());
      } else if (dr.kind == DialogKind::kBrowsePinyonFolder || dr.kind == DialogKind::kBrowsePinyonZip) {
        add_pinyon_source_path(dr.paths.front());
      } else if (dr.kind == DialogKind::kImportStfs || dr.kind == DialogKind::kImportContentFolder) {
        start_content_import(dr.paths);
      } else if (dr.kind == DialogKind::kInstallModZip || dr.kind == DialogKind::kInstallModFolder) {
        for (const auto& mp : dr.paths) {
          launcher::install_mod_source(mp, state.paths.mods_dir, state.banner_notice);
        }
        state.mods = launcher::scan_mods(state.paths.mods_dir);
        state.save_iso = launcher::evaluate_save_isolation(state.mods);
        state.active_tab = launcher::LauncherTab::kMods;
      } else if (dr.kind == DialogKind::kBrowseRadioMusic) {
        state.radio_cfg.music_folder = dr.paths.front();
        state.radio_tracks = launcher::scan_radio_music_folder(dr.paths.front());
        state.radio_status = std::to_string(state.radio_tracks.size()) + " audio track(s) found.";
        state.banner_notice = state.radio_status;
      } else if (dr.kind == DialogKind::kBrowseRadioLogo) {
        state.radio_cfg.logo_png_path = dr.paths.front();
        state.radio_logo_check = launcher::validate_station_logo_png(dr.paths.front());
        state.banner_notice = state.radio_logo_check.message;
      }
    }

    // Poll async setup runner, content STFS installer & Wi-Fi sync activity + live wheel axes
    state.setup_progress = setup_runner.snapshot();
    if (state.setup_progress.running || state.setup_progress.log_lines.size() != last_setup_logs ||
        state.setup_progress.fraction != last_setup_frac || state.setup_progress.stage_label != last_setup_stage) {
      last_setup_logs = state.setup_progress.log_lines.size();
      last_setup_frac = state.setup_progress.fraction;
      last_setup_stage = state.setup_progress.stage_label;
      dirty = true;
    }
    if (was_setup_running && !state.setup_progress.running && state.setup_progress.finished) {
      if (state.setup_progress.success) {
        launcher::read_play_config(state.paths.play_dir, state.play_cfg);
        state.banner_notice = "Setup complete.";
      } else if (!state.setup_progress.error_text.empty()) {
        state.banner_notice = state.setup_progress.error_text;
      }
      dirty = true;
    }
    was_setup_running = state.setup_progress.running;

    state.content_install = content_runner.snapshot();
    if (state.content_install.running || state.content_install.fraction != last_content_frac ||
        state.content_install.status_label != last_content_stage) {
      last_content_frac = state.content_install.fraction;
      last_content_stage = state.content_install.status_label;
      dirty = true;
    }
    if (was_content_running && !state.content_install.running && state.content_install.finished) {
      state.content_status = launcher::scan_content_packages(state.paths.state_dir, state.paths.mods_dir, state.paths.play_dir);
      if (!state.content_install.summary.empty()) state.banner_notice = state.content_install.summary;
      dirty = true;
    }
    was_content_running = state.content_install.running;

    if (wifi_server.running()) {
      auto act = wifi_server.recent_activity();
      if (act.size() != last_sync_activity_count) {
        last_sync_activity_count = act.size();
        state.sync_activity = std::move(act);
        dirty = true;
      }
    }

    if (live_wheel_poll && state.selected_device >= 0 && size_t(state.selected_device) < open_joys.size()) {
      SDL_Joystick* j = open_joys[size_t(state.selected_device)];
      auto ax = [&](int idx) {
        return (idx >= 0 && idx < SDL_GetNumJoystickAxes(j)) ? float(SDL_GetJoystickAxis(j, idx)) / 32767.0f : 0.0f;
      };
      state.live_steer = ax(std::max(0, state.wheel_cfg.steer.axis_index));
      state.live_throttle = std::clamp((ax(std::max(1, state.wheel_cfg.throttle.axis_index)) + 1.0f) * 0.5f, 0.0f, 1.0f);
      state.live_brake = std::clamp((ax(std::max(2, state.wheel_cfg.brake.axis_index)) + 1.0f) * 0.5f, 0.0f, 1.0f);
      if (std::fabs(state.live_steer - last_live_steer) > 0.004f ||
          std::fabs(state.live_throttle - last_live_throttle) > 0.004f ||
          std::fabs(state.live_brake - last_live_brake) > 0.004f) {
        last_live_steer = state.live_steer;
        last_live_throttle = state.live_throttle;
        last_live_brake = state.live_brake;
        dirty = true;
      }
    }

    // Skip rendering completely while minimized/occluded or clean so MoltenVK / DXGI never blocks while backgrounded.
    if (window_minimized || window_occluded || !dirty) {
      continue;
    }

    int win_w = int(width), win_h = int(height);
    int px_w = int(width), px_h = int(height);
    SDL_GetWindowSize(window, &win_w, &win_h);
    SDL_GetWindowSizeInPixels(window, &px_w, &px_h);
    if (px_w <= 0 || px_h <= 0) continue;

    const float pixel_density = win_w > 0 ? float(px_w) / float(win_w) : 1.0f;
    const float display_scale = std::max(1.0f, SDL_GetWindowDisplayScale(window));
    const float size_scale = std::min(float(px_w) / 1920.0f, float(px_h) / 1080.0f);
    const float dpi = std::clamp(std::max({pixel_density, display_scale, size_scale}) * user_zoom, 0.75f, 3.0f);
    launcher::build_launcher_ui(ui_list, state, float(px_w), float(px_h), hitboxes, dpi);
    upload_launcher_ui_atlas(renderer, !atlas_synced);
    atlas_synced = true;
    renderer.set_ui(ui_list.items());
    std::string ferr;
    if (!renderer.draw_frame(carview::FrameConstants{}, ferr)) break;
    dirty = false;
  }

  ui_audio.shutdown();
  setup_runner.cancel();
  content_runner.cancel();
  wifi_server.stop();
  for (SDL_Joystick* j : open_joys) SDL_CloseJoystick(j);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
