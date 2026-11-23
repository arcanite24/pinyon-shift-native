// carview --bake: the showroom over carbake's output, windowed or headless,
// the reference mode, and the drive mode (carview --drive, below).
//
//   carview --bake <dir> --car <media> [--car-id N] [--colour N] [--livery] [--all-variants]
//           [--size WxH] [--yaw deg] [--pitch deg] [--distance m] [--msaa N]
//           [--headless --out file.png [--mask mask.png] [--write-reference cam.json]]
//           [--frames N] [--validation] [--verbose]
//   carview --bake <dir> --reference <car_id.json> [--car <media>] --out file.png
//           [--mask mask.png] [--against ref.png --against-mask ref_mask.png]
//
// The reference mode (AUTOSHOW_REFERENCE schema v1 or v2) renders at the
// reference's camera, size and paint, writes the PNG and the car-coverage
// mask (255 inside), and prints the M1 metric (silhouette IoU, masked PSNR,
// edge overlap) against the reference's own image and mask, or --against. --write-reference
// saves an orbit camera in the reference schema (for checking the mode).

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <random>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include "achievements.h"
#include "audio/bake.h"
#include "audio/radio.h"
#include "audio/xml.h"
#include "benchmark.h"
#include "display_hdr_win.h"
#include "drive_helpers.h"
#include "fmv_player.h"
#include "frame_inspector.h"
#include "fh1data/cjk_font.h"
#include "game_hud.h"
#include "rally_hub.h"
#include "story_intro.h"
#include "ui_widgets.h"
#include "hud_scene.h"
#include "hud.h"
#include "livery_editor.h"
#include "photo_mode.h"
#include "platform_support.h"
#include "perf_overlay.h"
#include "process_time.h"
#include "render_modern.h"
#include "replay_view.h"
#include "reference.h"
#include "renderer.h"
#include "showroom.h"
#include "touch_controls.h"
#include "traffic_view.h"
#include "garage/rally_kit.h"
#include "race/rally_championship.h"
#include "race/rally_codriver.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/cameras.h"
#include "drive/controls.h"
#include "drive/rumble.h"
#include "drive/ai_driver.h"
#include "drive/contact.h"
#include "drive/driving_line.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "drive/rewind.h"
#include "drive/save.h"
#include "drive/vehicle.h"
#include "world/road_follow.h"
#include "world/smashables.h"
#include "world/world_anim.h"
#include "world/stream.h"
#include "world/vfx.h"
#include "world/world.h"
#include "contact_log.h"
#include "safe_reset.h"
#include "race_view.h"
#include "club_view.h"
#include "map_view.h"
#include "fh1data/archive.h"
#include "fh1data/spa.h"
#include "fh1data/stringtables.h"
#include "fh1data/vfont.h"
#include "fh1data/xds.h"
#include "freeroam/club.h"
#include "freeroam/collectibles.h"
#include "freeroam/festival_ai.h"
#include "freeroam/outposts.h"
#include "freeroam/road_discovery.h"
#include "freeroam/skills.h"
#include "freeroam/speed_zones.h"
#include "race/career.h"
#include "profile/drive_save.h"
#include "garage/garage.h"
#include "upgrade_shop.h"
#include "autoshow.h"
#include "hub_scene.h"
#include "garage/pi_calc.h"
#include "career_page.h"
#include "events_page.h"
#include "cars/mods.h"
#include "audio_host.h"
#include "settings.h"
#include "settings_page.h"
#include "slice12_runtime.h"
#include "mods_page.h"
#include "overlay_input.h"
#include "save_sync_ui.h"
#include "ui4_chrome.h"
#include "traffic/gps.h"
#include "traffic/nav.h"

namespace carview {

namespace {

constexpr float kPi = 3.14159265358979f;

struct V3 {
  float x, y, z;
};
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross3(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) { return mul(a, 1.0f / std::sqrt(dot3(a, a))); }

void multiply4(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
}

struct Orbit {
  float yaw = 145.0f, pitch = 12.0f, distance = 0;
};

// Our lighting's free settings; --light name=value,... overrides them. The
// defaults were fitted to the M1 references (median masked PSNR over the
// first eight Autoshow captures, with the diffuse light kept neutral so
// white paint stays white): a dim stage with
// blue reflections, almost no key light.
struct LightSettings {
  float exposure = 0.4f, environment = 0.586f, key = 0.076f, fill = 1.0f;
  float tint[3] = {0.88f, 0.9f, 0.95f};
  float reflect[3] = {0.512f, 0.9f, 1.775f};  // the stage's blue lights
  float metallic = 0.273f;
};
LightSettings g_light;

bool parse_light(const std::string& text) {
  std::stringstream list(text);
  for (std::string item; std::getline(list, item, ',');) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) return false;
    const std::string name = item.substr(0, eq);
    const float value = std::strtof(item.c_str() + eq + 1, nullptr);
    if (name == "exposure") g_light.exposure = value;
    else if (name == "env") g_light.environment = value;
    else if (name == "key") g_light.key = value;
    else if (name == "fill") g_light.fill = value;
    else if (name == "r") g_light.tint[0] = value;
    else if (name == "g") g_light.tint[1] = value;
    else if (name == "b") g_light.tint[2] = value;
    else if (name == "rr") g_light.reflect[0] = value;
    else if (name == "rg") g_light.reflect[1] = value;
    else if (name == "rb") g_light.reflect[2] = value;
    else if (name == "metal") g_light.metallic = value;
    else return false;
  }
  return true;
}

void lighting(ShowroomFrame& f, bool livery) {
  const V3 key = norm({0.35f, 1.0f, -0.45f}), fill = norm({-0.7f, 0.35f, 0.6f});
  f.key_light[0] = key.x;
  f.key_light[1] = key.y;
  f.key_light[2] = key.z;
  f.key_light[3] = g_light.key;
  f.fill_light[0] = fill.x;
  f.fill_light[1] = fill.y;
  f.fill_light[2] = fill.z;
  f.fill_light[3] = g_light.fill;
  f.options[0] = g_light.exposure;
  f.options[1] = g_light.environment;
  f.options[2] = livery ? 1.0f : 0.0f;
  f.options[3] = g_light.metallic;
  for (int i = 0; i < 3; ++i) {
    f.tint[i] = g_light.tint[i];
    f.reflect_tint[i] = g_light.reflect[i];
  }
  f.tint[3] = f.reflect_tint[3] = 1.0f;
}

ShowroomFrame orbit_frame(const Showroom& s, const Orbit& o, float aspect, bool livery) {
  const auto& lo = s.bounds_min();
  const auto& hi = s.bounds_max();
  const V3 low{lo[0], lo[1], lo[2]}, high{hi[0], hi[1], hi[2]};
  const V3 centre = mul(add(low, high), 0.5f);
  const float radius = std::sqrt(dot3(sub(high, low), sub(high, low))) * 0.5f;
  const float fovy = 35.0f * kPi / 180.0f;
  const float distance = o.distance > 0 ? o.distance : radius / std::tan(fovy / 2) * 0.8f;
  const float yaw = o.yaw * kPi / 180.0f, pitch = o.pitch * kPi / 180.0f;
  const V3 eye = add(centre, mul(V3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)},
                                 distance));
  const V3 f = norm(sub(centre, eye));
  const V3 side = norm(cross3(f, {0, 1, 0}));
  const V3 u = cross3(side, f);
  const float view[16] = {side.x, u.x, -f.x, 0, side.y, u.y, -f.y, 0, side.z, u.z, -f.z, 0,
                          -dot3(side, eye), -dot3(u, eye), dot3(f, eye), 1};
  const float near_z = std::max(0.05f, distance - radius * 2), far_z = distance + radius * 2;
  const float t = 1.0f / std::tan(fovy / 2);
  float projection[16] = {};
  projection[0] = t / aspect;
  projection[5] = -t;  // Vulkan clip: +y down
  projection[10] = far_z / (near_z - far_z);
  projection[11] = -1;
  projection[14] = near_z * far_z / (near_z - far_z);
  ShowroomFrame frame{};
  multiply4(projection, view, frame.view_projection);
  std::memcpy(frame.transparent_view_projection, frame.view_projection, sizeof frame.view_projection);
  frame.eye[0] = eye.x;
  frame.eye[1] = eye.y;
  frame.eye[2] = eye.z;
  frame.eye[3] = 1;
  lighting(frame, livery);
  return frame;
}

// Interleaved stereo float as a 16-bit WAV, clipped to full scale.
bool write_wav(const std::filesystem::path& path, int rate, const std::vector<float>& stereo) {
  std::ofstream out(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t bytes = uint32_t(stereo.size() * 2);
  out.write("RIFF", 4);
  u32(36 + bytes);
  out.write("WAVEfmt ", 8);
  u32(16), u16(1), u16(2), u32(uint32_t(rate)), u32(uint32_t(rate) * 4), u16(4), u16(16);
  out.write("data", 4);
  u32(bytes);
  for (float x : stereo) u16(uint16_t(int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767))));
  return bool(out);
}

bool write_png(const std::string& path, uint32_t w, uint32_t h, int channels, const uint8_t* data) {
  return stbi_write_png(path.c_str(), int(w), int(h), channels, data, int(w) * channels) != 0;
}

// The game's UI fonts (FORMAT-FONTS) from media/ui/Fonts.zip: A, B and E,
// the Cyrillic variants (strict supersets). False, with the stroke font
// kept, when the archive or a font is missing.
bool load_ui_fonts(const std::filesystem::path& game_root) {
  if (game_root.empty() || carview::ui::vector_fonts_loaded()) return carview::ui::vector_fonts_loaded();
  std::string why;
  auto zip = fh1::Archive::open(game_root / "media" / "ui" / "Fonts.zip", &why);
  if (!zip) {
    std::fprintf(stderr, "carview: note: no UI fonts (%s): the stroke font\n", why.c_str());
    return false;
  }
  if (!carview::ui::load_fonts_from_zip(*zip)) {
    std::fprintf(stderr, "carview: note: core vector fonts failed to load from Fonts.zip\n");
  }
  // The game's UI textures by path, on first use (FORMAT-UISCENES).
  carview::set_ui_textures([root = game_root](const std::string& path, std::array<float, 4>& uv) {
    if (carview::ui::atlas_find(path, uv)) return true;
    static std::unique_ptr<fh1::Archive> zip;
    static std::set<std::string> failed;
    static bool opened = false;
    if (failed.count(path)) return false;
    static std::unique_ptr<fh1::Archive> other;
    if (!opened) {
      opened = true;
      std::string e;
      zip = fh1::Archive::open(root / "media" / "ui" / "textures" / "Horizon.zip", &e);
      other = fh1::Archive::open(root / "media" / "ui" / "Textures.zip", &e);
    }
    // "T:" names a Textures.zip member, else Horizon.zip's (or loose Rally/DLC xds fallback).
    const bool t = path.rfind("T:", 0) == 0;
    const std::string rel = (t ? path.substr(2) : path) + ".xds";
    fh1::Archive* a = t ? other.get() : zip.get();
    fh1::ReadResult r = a ? a->read(rel) : fh1::ReadResult{};
    if (!r.ok() && !t) {
      // Fallback: loose textures under game_root or mounted Rally DLC directory (1600_pri_65).
      std::error_code ec;
      const std::filesystem::path loose = root / "media" / "ui" / "textures" / "Horizon" / rel;
      if (std::filesystem::is_regular_file(loose, ec)) {
        std::ifstream f(loose, std::ios::binary);
        r.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      } else {
        // Scan sibling .local/dlc or game_root/../dlc for Rally DLC 1600_pri_65 loose xds.
        static std::filesystem::path rally_tex_dir;
        static bool rally_probed = false;
        if (!rally_probed) {
          rally_probed = true;
          const std::filesystem::path bases[] = {
              root / ".." / ".." / "dlc",
              root / ".." / "dlc",
              root / "dlc",
          };
          for (const auto& base : bases) {
            if (!std::filesystem::exists(base, ec)) continue;
            for (const auto& ent : std::filesystem::recursive_directory_iterator(base, ec)) {
              if (ent.is_regular_file(ec) && ent.path().filename() == "CoDriverIconSet.xds") {
                rally_tex_dir = ent.path().parent_path().parent_path().parent_path();
                break;
              }
            }
            if (!rally_tex_dir.empty()) break;
          }
        }
        if (!rally_tex_dir.empty()) {
          const std::filesystem::path p = rally_tex_dir / rel;
          if (std::filesystem::is_regular_file(p, ec)) {
            std::ifstream f(p, std::ios::binary);
            r.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
          }
        }
      }
    }
    const fh1::XdsResult x = !r.data.empty() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
    if (x.levels.empty() || !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
      failed.insert(path);
      return false;
    }
    return true;
  });
  // The UI scenes by name, on first use (FORMAT-UISCENES: media/UI.zip
  // Scenes/ui4/<name>.fbf and .bsg).
  carview::set_ui_scenes([root = game_root](const std::string& name) -> const fh1::UiScene* {
    static std::unique_ptr<fh1::Archive> zip;
    static std::map<std::string, std::unique_ptr<fh1::UiScene>> cache;
    static bool opened = false;
    if (auto it = cache.find(name); it != cache.end()) return it->second.get();
    if (!opened) {
      opened = true;
      std::string e;
      zip = fh1::Archive::open(root / "media" / "UI.zip", &e);
    }
    auto& slot = cache[name];
    if (!zip) return nullptr;
    auto fbf = zip->read("Scenes/ui4/" + name + ".fbf");
    auto bsg = zip->read("Scenes/ui4/" + name + ".bsg");
    auto bgf = zip->read("Scenes/ui4/" + name + ".bgf");
    auto sc = std::make_unique<fh1::UiScene>();
    std::string e;
    if (fbf.ok() && bsg.ok() && fh1::parse_ui_fbf(fbf.data, *sc, &e) && fh1::parse_ui_bsg(bsg.data, *sc, &e)) {
      if (bgf.ok()) sc->has_bgf = fh1::parse_ui_bgf(bgf.data, sc->bgf, nullptr);
      slot = std::move(sc);
    } else
      std::fprintf(stderr, "carview: note: UI scene %s not loaded%s%s\n", name.c_str(), e.empty() ? "" : ": ", e.c_str());
    return slot.get();
  });
  // The brushes' tape masks (FORMAT-UISCENES: HORIZON\DIRTMASKS in
  // media/ui/textures/Horizon.zip) into the UI atlas.
  if (auto hz = fh1::Archive::open(game_root / "media" / "ui" / "textures" / "Horizon.zip", &why)) {
    for (const auto& [key, member] : {std::pair{"tape.smudge", "DIRTMASKS/SMUDGETAPE01.xds"},
                                      std::pair{"tape.crisp", "DIRTMASKS/CRISPTAPE01.xds"}}) {
      auto r = hz->read(member);
      const fh1::XdsResult x = r.ok() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
      std::array<float, 4> uv;
      if (x.levels.empty() ||
          !carview::ui::atlas_image(key, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv))
        std::fprintf(stderr, "carview: note: UI brush %s not loaded: flat brushes\n", member);
    }
  }
  return carview::ui::vector_fonts_loaded();
}

// The UI atlas's changed rectangles to the renderer (`full`: upload the whole
// atlas once on a fresh Renderer after a car/race reload).
void upload_ui_atlas(Renderer& renderer, bool full = false) {
  const auto& a = carview::ui::atlas_pixels();
  if (full) {
    carview::ui::atlas_take_dirty();
    renderer.ui_atlas_put(0, 0, carview::ui::kAtlasSize, carview::ui::kAtlasSize, a);
    return;
  }
  std::vector<uint8_t> rows;
  for (const auto& [x, y, w, h] : carview::ui::atlas_take_dirty()) {
    rows.resize(size_t(w) * h * 4);
    for (uint32_t r = 0; r < h; ++r)
      std::memcpy(&rows[size_t(r) * w * 4], &a[(size_t(y + r) * carview::ui::kAtlasSize + x) * 4], size_t(w) * 4);
    renderer.ui_atlas_put(x, y, w, h, rows);
  }
}

// A menu page (HudState's menu fields) on the UI layer, 1280 x 720, to a PNG.
bool write_menu_png(const HudState& st, const std::string& path, const carview::CareerBoard* board = nullptr) {
  carview::DriveHud d;
  d.career = board;
  d.driving = false;
  d.menu_title = st.menu_title;
  d.menu = st.menu;
  d.menu_selected = st.menu_selected;
  if (st.menu_title == "paused") {
    // The pause menu's panels, with a sample career.
    d.credits = 4075645, d.band_name = "ORANGE WRISTBAND", d.band_next = "Purple", d.band_level = 4;
    d.band_collect_ready = true, d.popularity_rank = 142;
    d.car_name = "Nissan Skyline GT-R V-spec II", d.car_class = "B", d.car_pi = 584;
  }
  carview::ui::List list;
  // A backdrop for the glass panel.
  list.rect(0, 0, 1280, 720, carview::ui::rgba(70, 92, 120), 0, carview::ui::rgba(150, 128, 96));
  carview::build_drive_hud(list, d, 1280, 720);
  std::vector<uint8_t> rgba;
  carview::ui::rasterize(list.items(), rgba, 1280, 720);
  return write_png(path, 1280, 720, 4, rgba.data());
}

bool read_png(const std::string& path, int channels, uint32_t w, uint32_t h, std::vector<uint8_t>& out,
              std::string& error) {
  int x = 0, y = 0, n = 0;
  uint8_t* data = stbi_load(path.c_str(), &x, &y, &n, channels);
  if (!data) {
    error = "cannot read " + path;
    return false;
  }
  if (uint32_t(x) != w || uint32_t(y) != h) {
    stbi_image_free(data);
    error = path + " is " + std::to_string(x) + "x" + std::to_string(y) + ", not " + std::to_string(w) + "x" +
            std::to_string(h);
    return false;
  }
  out.assign(data, data + size_t(x) * size_t(y) * size_t(channels));
  stbi_image_free(data);
  return true;
}

std::string mask_path_for(const std::string& out) {
  const std::filesystem::path p(out);
  return (p.parent_path() / (p.stem().string() + "_mask.png")).string();
}

void usage() {
  std::fprintf(stderr,
               "usage: carview --bake <dir> --car <media> [--colour N] [--livery] [--car-id N] [--all-variants]\n"
               "               [--size WxH] [--yaw deg] [--pitch deg] [--distance m] [--msaa N]\n"
               "               [--headless --out file.png [--mask mask.png]] [--frames N] [--validation]\n"
               "       carview --bake <dir> --reference <car.json> [--car <media>] --out file.png\n"
               "               [--mask mask.png] [--against ref.png --against-mask ref_mask.png]\n");
}

}  // namespace

int run_showroom(int argc, char** argv) {
  std::filesystem::path bake_dir, reference_path;
  std::string car, out, mask_out, against, against_mask, write_reference;
  uint32_t width = 1280, height = 720, msaa = 4, max_frames = 0;
  bool headless = false, validation = std::getenv("SHIFT_VALIDATION") != nullptr, verbose = false;
  bool coverage_mask = false;
  Orbit orbit;
  ShowroomOptions options;
  int colour = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--headless") headless = true;
    else if (a == "--validation") validation = true;
    else if (a == "--verbose") verbose = true;
    else if (a == "--livery") options.livery_overlay = true;
    else if (a == "--all-variants") options.all_variants = true;
    else if (a == "--car-id" && (v = next())) options.car_id = std::atoll(v);
    else if (a == "--coverage-mask") coverage_mask = true;
    else if (a == "--bake" && (v = next())) bake_dir = v;
    else if (a == "--car" && (v = next())) car = v;
    else if (a == "--reference" && (v = next())) reference_path = v;
    else if (a == "--out" && (v = next())) out = v;
    else if (a == "--mask" && (v = next())) mask_out = v;
    else if (a == "--against" && (v = next())) against = v;
    else if (a == "--against-mask" && (v = next())) against_mask = v;
    else if (a == "--write-reference" && (v = next())) write_reference = v;
    else if (a == "--colour" && (v = next())) colour = std::atoi(v);
    else if (a == "--frames" && (v = next())) max_frames = uint32_t(std::atoi(v));
    else if (a == "--msaa" && (v = next())) msaa = uint32_t(std::atoi(v));
    else if (a == "--light" && (v = next())) {
      if (!parse_light(v)) {
        std::fprintf(stderr, "carview: --light takes name=value,... (exposure, env, key, fill, r, g, b, rr, rg, rb, metal)\n");
        return 2;
      }
    }
    else if (a == "--yaw" && (v = next())) orbit.yaw = float(std::atof(v));
    else if (a == "--pitch" && (v = next())) orbit.pitch = float(std::atof(v));
    else if (a == "--distance" && (v = next())) orbit.distance = float(std::atof(v));
    else if (a == "--size" && (v = next())) {
      if (std::sscanf(v, "%ux%u", &width, &height) != 2 || !width || !height) {
        usage();
        return 2;
      }
    } else {
      usage();
      return 2;
    }
  }
  std::string error;
  Reference reference;
  const bool reference_mode = !reference_path.empty();
  if (reference_mode) {
    if (!read_reference(reference_path, reference, error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      return 1;
    }
    if (car.empty()) car = reference.media_name;
    // The row shown (its stock parts and livery): the reference's car id.
    if (options.car_id == 0) options.car_id = reference.car_id;
    if (colour < 0 && reference.has_paint)
      options.match_rgb = int64_t(reference.paint_rgb[0]) * 65536 + reference.paint_rgb[1] * 256 +
                          reference.paint_rgb[2];
    width = reference.width;
    height = reference.height;
    headless = true;
    if (out.empty()) out = car + ".png";
    if (mask_out.empty()) mask_out = mask_path_for(out);
    // Compare with the reference's own capture unless told otherwise: the
    // car-only replay when there is one (no stage behind the glass),
    // else the frame.
    const std::filesystem::path& target =
        !reference.image_car_only.empty() && std::filesystem::exists(reference.image_car_only)
            ? reference.image_car_only
            : reference.image;
    if (against.empty() && !target.empty() && !reference.mask.empty() && std::filesystem::exists(target) &&
        std::filesystem::exists(reference.mask)) {
      against = target.string();
      against_mask = reference.mask.string();
    }
  }
  if (bake_dir.empty() || car.empty() || (against.empty() != against_mask.empty())) {
    usage();
    return 2;
  }
  if (headless && out.empty()) out = car + ".png";
  if (colour >= 0) options.colour = size_t(colour);

  // The headless renders are drawn and written the same way every run.
  SDL_Window* window = nullptr;
  if (!headless) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
      std::fprintf(stderr, "carview: SDL_Init: %s\n", SDL_GetError());
      return 1;
    }
    window = SDL_CreateWindow(("carview - " + car).c_str(), int(width), int(height),
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
      std::fprintf(stderr, "carview: SDL_CreateWindow: %s\n", SDL_GetError());
      SDL_Quit();
      return 1;
    }
  }
  int status = 0;
  {
    Renderer renderer;
    Showroom showroom(renderer);
    if (const char* env_root = std::getenv("SHIFT_GAME_ROOT"); env_root && *env_root) {
      const auto xpr = std::filesystem::path(env_root) / "media" / "tracks" / "uiautoshow" / "staticcarcubemap.xpr";
      if (std::filesystem::exists(xpr)) showroom.set_environment_xpr(xpr);
    }
    if (!showroom.load(bake_dir, car, options, error) || !renderer.init({window, validation, msaa}, error) ||
        !showroom.upload(error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      status = 1;
    } else {
      const auto& info = showroom.car();
      const auto& lo = showroom.bounds_min();
      const auto& hi = showroom.bounds_max();
      std::printf("%s (%s): %zu draws, %zu vertices, %zu triangles, %zu textures; colour %zu of %zu\n",
                  info.media_name.c_str(), info.display_name.c_str(), showroom.draw_count(),
                  showroom.vertex_count(), showroom.triangle_count(), showroom.texture_count(),
                  info.colours.empty() ? 0 : std::min(options.colour, info.colours.size() - 1),
                  info.colours.size());
      std::printf("bounds: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f m\n", lo[0], hi[0], lo[1], hi[1], lo[2],
                  hi[2]);
      for (const auto& n : showroom.notes()) std::printf("note: %s\n", n.c_str());
      if (verbose) {
        for (const auto& p : showroom.drawn_parts()) std::printf("drawn: %s\n", p.c_str());
        for (const auto& p : showroom.skipped_parts()) std::printf("skipped: %s\n", p.c_str());
      }
      if (headless) {
        ShowroomFrame frame;
        if (reference_mode) {
          frame = ShowroomFrame{};
          // Direct3D clip (+y up) to Vulkan clip (+y down): negate row 1.
          std::memcpy(frame.view_projection, reference.clip_from_car, sizeof frame.view_projection);
          std::memcpy(frame.transparent_view_projection, reference.transparent_clip_from_car,
                      sizeof frame.transparent_view_projection);
          for (int c = 0; c < 4; ++c) {
            frame.view_projection[c * 4 + 1] = -frame.view_projection[c * 4 + 1];
            frame.transparent_view_projection[c * 4 + 1] = -frame.transparent_view_projection[c * 4 + 1];
          }
          float eye[3] = {0, 0, 0};
          if (!clip_eye(reference.clip_from_car, eye))
            std::fprintf(stderr, "carview: the reference matrix has no camera centre; lighting from the origin\n");
          frame.eye[0] = eye[0];
          frame.eye[1] = eye[1];
          frame.eye[2] = eye[2];
          frame.eye[3] = 1;
          lighting(frame, options.livery_overlay);
        } else {
          frame = orbit_frame(showroom, orbit, float(width) / float(height), options.livery_overlay);
          if (!write_reference.empty()) {
            // The orbit camera as a v0 reference (Direct3D clip: row 1 negated back).
            std::FILE* f = std::fopen(write_reference.c_str(), "wb");
            if (!f) {
              std::fprintf(stderr, "carview: cannot write %s\n", write_reference.c_str());
              status = 1;
            } else {
              const cars::bake::Colour shown =
                  info.colours.empty() ? cars::bake::Colour{}
                                       : info.colours[std::min(options.colour, info.colours.size() - 1)];
              std::fprintf(f,
                           "{\n  \"schema\": \"shift-engine.m1-reference.v1\",\n  \"car_id\": %lld,\n"
                           "  \"media_name\": \"%s\",\n  \"width\": %u,\n  \"height\": %u,\n"
                           "  \"paint_rgb\": [%u, %u, %u],\n  \"clip_from_car\": [",
                           static_cast<long long>(info.car_id), info.media_name.c_str(), width, height,
                           (shown.rgb >> 16) & 0xFFu, (shown.rgb >> 8) & 0xFFu, shown.rgb & 0xFFu);
              for (int i = 0; i < 16; ++i)
                std::fprintf(f, "%s%.9g", i ? ", " : "",
                             double(i % 4 == 1 ? -frame.view_projection[i] : frame.view_projection[i]));
              std::fprintf(f, "]\n}\n");
              std::fclose(f);
            }
          }
        }
        showroom.set_frame(frame);
        // AUTOSHOW_REFERENCE's mask: the pixels a car-only replay changes
        // by 6/255 or more in any channel against an empty one. Ours: every
        // pixel any car draw touches (any MSAA sample), glass included,
        // whatever its colour, so glass over the grey stage and shading near
        // the background grey do not cut holes in the mask (the spec side's
        // M1 rule, 2026-11-20). The colour image is still drawn over the
        // capture's empty background (its top-left pixel). --coverage-mask
        // keeps the default background.
        uint8_t background[3] = {0, 0, 0};
        if (reference_mode && !coverage_mask && !against.empty()) {
          std::vector<uint8_t> target;
          std::string ignored;
          if (read_png(against, 4, width, height, target, ignored)) {
            for (int c = 0; c < 3; ++c) background[c] = target[size_t(c)];
            showroom.set_background(background[0] / 255.0f, background[1] / 255.0f, background[2] / 255.0f);
          }
        }
        std::vector<uint8_t> rgba, mask;
        if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error) ||
            ((!mask_out.empty() || !against.empty()) ? !showroom.render_mask(width, height, mask, error)
                                                      : false)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
        } else {
          // The background is opaque; the alpha channel holds blending
          // leftovers, so write it as 255.
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(out, width, height, 4, rgba.data())) {
            std::fprintf(stderr, "carview: cannot write %s\n", out.c_str());
            status = 1;
          } else {
            std::printf("rendered %ux%u on %s (%ux MSAA), wrote %s\n", width, height,
                        renderer.device_name().c_str(), renderer.samples(), out.c_str());
          }
          if (status == 0 && !mask_out.empty()) {
            if (!write_png(mask_out, width, height, 1, mask.data())) {
              std::fprintf(stderr, "carview: cannot write %s\n", mask_out.c_str());
              status = 1;
            } else {
              size_t covered = 0;
              for (uint8_t m : mask) covered += m != 0;
              std::printf("mask: %zu of %zu pixels, wrote %s\n", covered, mask.size(), mask_out.c_str());
            }
          }
          if (status == 0 && !against.empty()) {
            std::vector<uint8_t> ref_rgba, ref_mask;
            if (!read_png(against, 4, width, height, ref_rgba, error) ||
                !read_png(against_mask, 1, width, height, ref_mask, error)) {
              std::fprintf(stderr, "carview: %s\n", error.c_str());
              status = 1;
            } else {
              const Metric m = compare(ref_rgba, ref_mask, rgba, mask, width, height);
              std::printf("metric %s: iou %.4f masked_psnr %.2f dB edge_overlap %.4f (reference %llu px, "
                          "engine %llu px)\n",
                          car.c_str(), m.iou, m.masked_psnr, m.edge_overlap,
                          static_cast<unsigned long long>(m.reference_pixels),
                          static_cast<unsigned long long>(m.engine_pixels));
            }
          }
        }
      } else {
        std::printf("device: %s (%ux MSAA)\n", renderer.device_name().c_str(), renderer.samples());
        bool dragging = false, running = true;
        float zoom = 1.0f;
        std::vector<SDL_Gamepad*> pads;
        uint64_t last = SDL_GetTicksNS();
        for (uint32_t n = 0; running && (!max_frames || n < max_frames); ++n) {
          SDL_Event e;
          while (SDL_PollEvent(&e)) {
            switch (e.type) {
              case SDL_EVENT_QUIT: running = false; break;
              case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) running = false;
                if (e.key.key == SDLK_L) options.livery_overlay = !options.livery_overlay;
                break;
              case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) dragging = true;
                break;
              case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) dragging = false;
                break;
              case SDL_EVENT_MOUSE_MOTION:
                if (dragging) {
                  orbit.yaw -= e.motion.xrel * 0.3f;
                  orbit.pitch = std::clamp(orbit.pitch + e.motion.yrel * 0.3f, -85.0f, 85.0f);
                }
                break;
              case SDL_EVENT_MOUSE_WHEEL: zoom = std::clamp(zoom * std::pow(0.9f, e.wheel.y), 0.2f, 5.0f); break;
              case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(pad);
                break;
              default: break;
            }
          }
          const uint64_t now = SDL_GetTicksNS();
          const float dt = float(now - last) * 1e-9f;
          last = now;
          for (SDL_Gamepad* pad : pads) {
            auto axis = [&](SDL_GamepadAxis a) {
              const float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
              return std::fabs(v) < 0.15f ? 0.0f : v;
            };
            orbit.yaw -= (axis(SDL_GAMEPAD_AXIS_LEFTX) + axis(SDL_GAMEPAD_AXIS_RIGHTX)) * 120 * dt;
            orbit.pitch = std::clamp(
                orbit.pitch + (axis(SDL_GAMEPAD_AXIS_LEFTY) + axis(SDL_GAMEPAD_AXIS_RIGHTY)) * 90 * dt, -85.0f, 85.0f);
            const float z = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) - axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            zoom = std::clamp(zoom * std::pow(2.0f, z * dt), 0.2f, 5.0f);
          }
          int w = 0, h = 0;
          SDL_GetWindowSizeInPixels(window, &w, &h);
          Orbit view = orbit;
          if (view.distance <= 0) {
            const auto& lo = showroom.bounds_min();
            const auto& hi = showroom.bounds_max();
            const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
            view.distance = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f / std::tan(35.0f * kPi / 360.0f) * 0.8f;
          }
          view.distance *= zoom;
          showroom.set_frame(orbit_frame(showroom, view, h > 0 ? float(w) / float(h) : 1.0f, options.livery_overlay));
          if (!renderer.draw_frame(FrameConstants{}, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
        }
        for (SDL_Gamepad* pad : pads) {
          SDL_RumbleGamepad(pad, 0, 0, 0);
          SDL_CloseGamepad(pad);
        }
      }
      if (validation) std::printf("validation messages: %u\n", renderer.validation_messages());
      if (status == 0 && renderer.validation_messages()) status = 3;
    }
  }
  if (window) {
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
  return status;
}

namespace {

// Column-major 4x4 inverse of an affine matrix (3x3 linear part, then
// translation).
void invert_affine(const float* m, float* out) {
  const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
  const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
  const float id = det != 0 ? 1.0f / det : 0.0f;
  float inv[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                  (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                  (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};  // row-major
  std::memset(out, 0, 16 * sizeof(float));
  for (int r = 0; r < 3; ++r)
    for (int col = 0; col < 3; ++col) out[col * 4 + r] = inv[r * 3 + col];
  for (int r = 0; r < 3; ++r)
    out[12 + r] = -(inv[r * 3 + 0] * m[12] + inv[r * 3 + 1] * m[13] + inv[r * 3 + 2] * m[14]);
  out[15] = 1;
}
V3 transform(const float* m, V3 p, float w) {
  return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12] * w, m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13] * w,
          m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] * w};
}

namespace {
float g_cam_near = 0.25f, g_cam_far = 16000.0f, g_cam_proj00 = 1.0f, g_cam_proj11 = -1.0f;
float g_cam_vp_unjittered[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

std::vector<std::string> scan_lut_files(const std::filesystem::path& state_root) {
  std::vector<std::string> out;
  if (state_root.empty()) return out;
  const std::filesystem::path dir = state_root / "luts";
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (entry.path().extension() == ".cube" || entry.path().extension() == ".CUBE") {
      out.push_back(entry.path().filename().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool apply_user_cube_lut(const std::string& lut_name, const std::filesystem::path& state_root, Renderer& renderer,
                         std::string& error) {
  if (lut_name.empty()) return false;
  std::filesystem::path p(lut_name);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec) && !state_root.empty()) {
    const std::filesystem::path in_luts = state_root / "luts" / p;
    if (std::filesystem::is_regular_file(in_luts, ec)) p = in_luts;
  }
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) {
    error = "cannot open .cube LUT: " + p.string();
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string text;
  if (sz > 0) {
    text.resize(size_t(sz));
    const size_t got = std::fread(text.data(), 1, text.size(), f);
    text.resize(got);
  }
  std::fclose(f);
  carview::modern::CubeLut cube;
  if (!carview::modern::parse_cube(text, cube, error)) return false;
  const auto rgba = carview::modern::cube_to_rgba8(cube);
  if (rgba.empty()) {
    error = "empty .cube LUT";
    return false;
  }
  renderer.set_post_lut(rgba, cube.size);
  return true;
}
}  // namespace

// A camera at eye looking at target, in the car's model frame.
ShowroomFrame look_frame(V3 eye, V3 target, V3 up, float aspect, bool livery, bool reverse_z = false,
                         float fov_degrees = 55.0f, float near_clip = 0.25f) {
  fov_degrees = carview::modern::effective_vfov(fov_degrees, aspect);
  const float fovy = fov_degrees * kPi / 180.0f;
  const V3 f = norm(sub(target, eye));
  const V3 side = norm(cross3(f, up));
  const V3 u = cross3(side, f);
  const float view[16] = {side.x, u.x, -f.x, 0, side.y, u.y, -f.y, 0, side.z, u.z, -f.z, 0,
                          -dot3(side, eye), -dot3(u, eye), dot3(f, eye), 1};
  const float near_z = near_clip, far_z = 16000.0f;  // the far ring reaches several km
  const float t = 1.0f / std::tan(fovy / 2);
  float projection[16] = {};
  projection[0] = t / aspect;
  projection[5] = -t;  // Vulkan clip: +y down
  projection[10] = far_z / (near_z - far_z);
  projection[11] = -1;
  projection[14] = near_z * far_z / (near_z - far_z);
  if (reverse_z) {
    // Reversed, with the far plane at infinity: depth = near / distance.
    projection[10] = 0;
    projection[14] = near_z;
  }
  g_cam_near = near_z;
  g_cam_far = far_z;
  g_cam_proj00 = projection[0];
  g_cam_proj11 = projection[5];
  ShowroomFrame frame{};
  multiply4(projection, view, frame.view_projection);
  std::memcpy(g_cam_vp_unjittered, frame.view_projection, sizeof g_cam_vp_unjittered);
  std::memcpy(frame.transparent_view_projection, frame.view_projection, sizeof frame.view_projection);
  frame.eye[0] = eye.x;
  frame.eye[1] = eye.y;
  frame.eye[2] = eye.z;
  frame.eye[3] = 1;
  lighting(frame, livery);
  return frame;
}

// Our chase camera (placeholder values until a camera spec): behind and
// above the car, following its heading but not its pitch and roll, so the
// body's motion on its springs shows. Built in the world and taken into the
// model frame, where the showroom draws.
// --view front: a fixed front three-quarter view from the car's left, low,
// for checking the wheels' steer and spin.
// The chase camera's pull-in (ours, until the camera spec): a ray from the
// look-at point to the eye against the ground's collision; when it meets a
// surface the eye moves in to 0.2 m short of it (at once), and eases back
// out over about a third of a second once clear. `pull` keeps the fraction
// of the full distance between frames (1 = full); `dt` is the time since
// the last frame.
// The chase camera's state between frames (ours): how far it is pulled in
// by the collision, its heading, which trails the car's, and a look-around
// angle from the right stick.
struct CameraPull {
  double fraction = 1;
  bool started = false;
  double yaw = 0;   // the camera's heading, radians
  double look = 0;  // added to it (right stick or look back), radians
  double look_target = 0;
};

std::string lower_ascii(std::string v) {
  for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
  return v;
}

// The signed angle from a to b, in (-pi, pi].
double angle_to(double a, double b) {
  const double d = std::remainder(b - a, 2 * 3.14159265358979323846);
  return d;
}

// A stick or trigger value rescaled from its inside to its outside deadzone
// to 0..1 and clamped, the sign kept (CAR_PHYSICS "Throttle, brake and
// steering from the pad": the rule is probable; the deadzone values are
// ours, ForzaProfile's defaults are not read).
double deadzone(double v, double inside, double outside) {
  const double m = std::clamp((std::fabs(v) - inside) / (outside - inside), 0.0, 1.0);
  return v < 0 ? -m : m;
}

ShowroomFrame chase_frame(const cars::bake::CarInfo& car, const drive::Vehicle& v, float aspect, bool front,
                          const drive::Ground* ground = nullptr, CameraPull* pull = nullptr, double dt = 0,
                          bool reverse_z = false) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(car, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  const drive::Vec3 p = v.position();
  const drive::Vec3 fwd = drive::rotate(v.orientation(), {0, 0, 1});
  const double car_yaw = std::atan2(fwd.x, fwd.z);
  // The chase heading trails the car's (time constant 0.2 s, ours) so a
  // slide or spin does not whip the view; the look angle eases to its
  // target in 0.1 s. Headless frames (dt of a second or more) settle.
  double yaw_d = car_yaw;
  if (pull) {
    if (!pull->started || dt <= 0) {
      pull->yaw = car_yaw;
      pull->look = pull->look_target;
      pull->started = true;
    } else {
      pull->yaw += angle_to(pull->yaw, car_yaw) * (1 - std::exp(-dt / 0.2));
      pull->look += (pull->look_target - pull->look) * (1 - std::exp(-dt / 0.1));
    }
    yaw_d = pull->yaw + pull->look;
  }
  const float yaw = float(front ? car_yaw : yaw_d);
  const V3 dir{std::sin(yaw), 0, std::cos(yaw)};
  const V3 base{float(p.x), float(p.y), float(p.z)};
  const V3 left{-dir.z, 0, dir.x};
  // Distance and height from the car's size (ours): 4.5 m back and 1.6 m
  // up for a 4.3 m, 1.3 m car.
  const auto& size = v.params().size;
  const float back = float(1.9 + 0.6 * size.z), up = float(0.6 + 0.77 * size.y);
  const V3 eye = front ? add(add(add(base, mul(dir, 4.0f)), mul(left, 3.0f)), V3{0, 0.9f, 0})
                       : add(add(base, mul(dir, -back)), V3{0, up, 0});
  const V3 target = front ? add(base, V3{0, 0.5f, 0})
                          : add(add(base, mul(dir, float(0.35 * size.z))), V3{0, 0.7f, 0});
  V3 eye_used = eye;
  if (ground && pull) {
    const drive::Vec3 from{target.x, target.y, target.z};
    const drive::Vec3 ray{double(eye.x) - target.x, double(eye.y) - target.y, double(eye.z) - target.z};
    const double reach = std::sqrt(ray.x * ray.x + ray.y * ray.y + ray.z * ray.z);
    double want = 1;
    drive::Hit hit;
    if (reach > 1e-6 && ground->raycast(from, ray * (1 / reach), reach, hit))
      want = std::clamp((hit.distance - 0.2) / reach, 0.1, 1.0);
    pull->fraction = want < pull->fraction ? want : pull->fraction + (want - pull->fraction) * std::min(1.0, dt * 3);
    const float f = float(pull->fraction);
    eye_used = add(target, mul(V3{eye.x - target.x, eye.y - target.y, eye.z - target.z}, f));
  }
  return look_frame(transform(world_to_model, eye_used, 1), transform(world_to_model, target, 1),
                    norm(transform(world_to_model, {0, 1, 0}, 0)), aspect, false, reverse_z);
}

// Scenic orbiting camera around the car (Title Screen backdrop & Pre-Race Grid showcase).
ShowroomFrame scenic_orbit_frame(const cars::bake::CarInfo& car, const drive::Vehicle& v, float aspect, double yaw_rad,
                                 float dist, float height, bool reverse_z) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(car, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  const drive::Vec3 p = v.position();
  const V3 base{float(p.x), float(p.y), float(p.z)};
  const V3 eye{base.x + dist * float(std::sin(yaw_rad)), base.y + height, base.z + dist * float(std::cos(yaw_rad))};
  const V3 target{base.x, base.y + 0.65f, base.z};
  return look_frame(transform(world_to_model, eye, 1), transform(world_to_model, target, 1),
                    norm(transform(world_to_model, {0, 1, 0}, 0)), aspect, false, reverse_z, 45.0f);
}

// Projects a world-frame point through the player car's current view_projection to screen px.
bool project_world_point(const ShowroomFrame& frame, const cars::bake::CarInfo& car, const drive::Vehicle& v, double wx,
                         double wy, double wz, float screen_w, float screen_h, float& sx, float& sy) {
  float m2w[16], w2m[16];
  drive::model_to_world(car, v, m2w);
  invert_affine(m2w, w2m);
  const V3 pm = transform(w2m, {float(wx), float(wy), float(wz)}, 1.0f);
  const float* vp = frame.view_projection;
  const float cx = vp[0] * pm.x + vp[4] * pm.y + vp[8] * pm.z + vp[12];
  const float cy = vp[1] * pm.x + vp[5] * pm.y + vp[9] * pm.z + vp[13];
  const float cw = vp[3] * pm.x + vp[7] * pm.y + vp[11] * pm.z + vp[15];
  if (cw <= 0.1f) return false;
  sx = (cx / cw * 0.5f + 0.5f) * screen_w;
  sy = (cy / cw * 0.5f + 0.5f) * screen_h;
  return true;
}

const char* difficulty_name(race::Difficulty d) {
  switch (d) {
    case race::Difficulty::kEasy: return "EASY";
    case race::Difficulty::kMed: return "MED";
    case race::Difficulty::kHard: return "HARD";
    case race::Difficulty::kPro: return "PRO";
    case race::Difficulty::kHardPlus: return "HARD+";
  }
  return "MED";
}

SDL_Scancode sdl_scancode_for(std::string_view name) {
  if (name.empty()) return SDL_SCANCODE_UNKNOWN;
  if (name == "Comma") return SDL_SCANCODE_COMMA;
  if (name == "Period") return SDL_SCANCODE_PERIOD;
  if (name == "LeftShift") return SDL_SCANCODE_LSHIFT;
  if (name == "RightShift") return SDL_SCANCODE_RSHIFT;
  if (name == "LeftCtrl") return SDL_SCANCODE_LCTRL;
  if (name == "RightCtrl") return SDL_SCANCODE_RCTRL;
  return SDL_GetScancodeFromName(std::string(name).c_str());
}

std::string sdl_key_name(SDL_Keycode k) {
  switch (k) {
    case SDLK_COMMA: return "Comma";
    case SDLK_PERIOD: return "Period";
    case SDLK_LSHIFT: return "LeftShift";
    case SDLK_RSHIFT: return "RightShift";
    case SDLK_LCTRL: return "LeftCtrl";
    case SDLK_RCTRL: return "RightCtrl";
    default: break;
  }
  const char* raw = SDL_GetKeyName(k);
  if (!raw || !*raw) return {};
  std::string out;
  for (const char* p = raw; *p; ++p)
    if (*p != ' ') out.push_back(*p);
  return out;
}

bool action_key_down(const bool* keys, const drive::KeyBindings& kb, drive::ControlAction act) {
  const auto& b = kb.map[size_t(act)];
  const SDL_Scancode s1 = sdl_scancode_for(b.primary);
  if (s1 != SDL_SCANCODE_UNKNOWN && keys[s1]) return true;
  const SDL_Scancode s2 = sdl_scancode_for(b.secondary);
  if (s2 != SDL_SCANCODE_UNKNOWN && keys[s2]) return true;
  return false;
}

drive::PadControl pad_control_from_button(Uint8 b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return drive::PadControl::kButtonA;
    case SDL_GAMEPAD_BUTTON_EAST: return drive::PadControl::kButtonB;
    case SDL_GAMEPAD_BUTTON_WEST: return drive::PadControl::kButtonX;
    case SDL_GAMEPAD_BUTTON_NORTH: return drive::PadControl::kButtonY;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return drive::PadControl::kLeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return drive::PadControl::kRightShoulder;
    case SDL_GAMEPAD_BUTTON_BACK: return drive::PadControl::kBack;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return drive::PadControl::kDPadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return drive::PadControl::kDPadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return drive::PadControl::kDPadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return drive::PadControl::kDPadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return drive::PadControl::kLeftStickClick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return drive::PadControl::kRightStickClick;
    default: return drive::PadControl::kNone;
  }
}

double pad_control_value(SDL_Gamepad* pad, drive::PadControl c, double dz_in, double dz_out) {
  auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
  switch (c) {
    case drive::PadControl::kLeftTrigger:
      return drive::apply_deadzone_unsigned(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), dz_in, dz_out);
    case drive::PadControl::kRightTrigger:
      return drive::apply_deadzone_unsigned(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), dz_in, dz_out);
    case drive::PadControl::kButtonA: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ? 1.0 : 0.0;
    case drive::PadControl::kButtonB: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ? 1.0 : 0.0;
    case drive::PadControl::kButtonX: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST) ? 1.0 : 0.0;
    case drive::PadControl::kButtonY: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH) ? 1.0 : 0.0;
    case drive::PadControl::kLeftShoulder:
      return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 1.0 : 0.0;
    case drive::PadControl::kRightShoulder:
      return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 1.0 : 0.0;
    default: return 0.0;
  }
}

// Plumbs live AssistSettings into drive::Vehicle and race::DifficultyAssists.
void apply_vehicle_assists(drive::Vehicle& vehicle, const drive::AssistSettings& assists) {
  drive::VehicleAssists va;
  va.braking = assists.braking;
  va.steering = assists.steering;
  va.traction_control = assists.traction_control;
  va.stability_control = assists.stability_control;
  va.shifting = assists.shifting;
  vehicle.set_assists(va);
}

race::DifficultyAssists to_difficulty_assists(const drive::AssistSettings& a) {
  race::DifficultyAssists da;
  da.rewind_on = a.rewind;
  da.braking = a.braking;
  da.steering = a.steering;
  da.tcs_on = a.traction_control;
  da.stm_on = a.stability_control;
  da.shifting = a.shifting;
  da.driving_line = a.driving_line;
  return da;
}

// SPEC-CAMERAS' rig for the drive: the tuning from camera.zip (v4's layers
// when installed), the car's CameraOverrides row and MAXData box from the
// player's files, the CoM and steering wheel from the car in use. False
// (with a note) when something is missing: the drive keeps the old chase.
bool load_camera_rig(const std::filesystem::path& game_root, const std::filesystem::path& bake_dir,
                     const cars::bake::CarInfo& info, const drive::Vehicle& v, drive::DriveCamera& rig,
                     std::string& note, bool camera_effects = true) {
  cars::Install install;
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  std::string ini, xml;
  drive::CameraTuning tuning;
  if (!cars::discover_install(game_root, state ? state : "", "", install, &note) ||
      !cars::load_camera_files(install, ini, xml, &note, camera_effects) ||
      !drive::parse_camera_settings(ini, tuning, &note) || !drive::parse_camera_physics(xml, tuning, &note))
    return false;
  // The box's z shift and the pristine box (the fallback) from the bake's rows.
  cars::CarPhysics physics;
  cars::PhysicsSettings settings_unused;
  {
    std::string name = lower_ascii(info.media_name);
    std::ifstream in(cars::mods::resolve(bake_dir / "cars" / (name + ".physics.txt")), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string e;
    if (!in || !cars::physics_from_text(ss.str(), physics, settings_unused, &e)) {
      note = "no physics rows for " + info.media_name;
      return false;
    }
  }
  const double shift = physics.get("Data_CarBody.BottomCenterWheelbasePosZ");
  drive::CameraCar car;
  cars::CarCameraRow row;
  std::string row_error;
  if (cars::load_car_camera(install, info.car_id, info.media_name, row, &row_error) && row.has_box) {
    car.box_min = {row.box[0], row.box[1], row.box[2] - shift};
    car.box_max = {row.box[3], row.box[4], row.box[5] - shift};
  } else {
    // SPEC-CAMERAS, Unknowns: failing MAXData, the pristine box.
    note = row_error + "; the pristine box instead";
    const char* k = "Data_CarBody.PristineBoundingBox";
    car.box_min = {physics.get(std::string(k) + "MinX"), physics.get(std::string(k) + "MinY"),
                   physics.get(std::string(k) + "MinZ") - shift};
    car.box_max = {physics.get(std::string(k) + "MaxX"), physics.get(std::string(k) + "MaxY"),
                   physics.get(std::string(k) + "MaxZ") - shift};
  }
  car.overrides = row.overrides;
  // The CoM: the body frame (ground-origin, FORMAT-CARS' pose) to the car
  // frame (the model origin, z forward).
  const drive::Vec3 b = v.params().centre_of_mass;
  const double c = std::cos(info.pitch), s = std::sin(info.pitch), y = b.y - info.lift;
  car.com = {b.x, c * y + s * b.z, -s * y + c * b.z};
  for (int k = 0; k < 3; ++k) car.steering_wheel[size_t(k)] = info.part_positions[9][size_t(k)];
  rig.set_tuning(tuning);
  rig.set_car(car);
  return true;
}

ShowroomFrame camera_view_frame(const drive::CameraView& view, const cars::bake::CarInfo& info,
                                const drive::Vehicle& v, float aspect, bool reverse_z) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(info, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  const V3 eye{float(view.eye[0]), float(view.eye[1]), float(view.eye[2])};
  const V3 at{float(view.eye[0] + view.forward[0]), float(view.eye[1] + view.forward[1]),
              float(view.eye[2] + view.forward[2])};
  const V3 up{float(view.up[0]), float(view.up[1]), float(view.up[2])};
  return look_frame(transform(world_to_model, eye, 1), transform(world_to_model, at, 1),
                    norm(transform(world_to_model, up, 0)), aspect, false, reverse_z, float(view.fov_degrees),
                    float(view.near_clip));
}

void apply_ndc_jitter(ShowroomFrame& f, float jx, float jy) {
  for (int c = 0; c < 4; ++c) {
    f.view_projection[c * 4 + 0] += jx * f.view_projection[c * 4 + 3];
    f.view_projection[c * 4 + 1] += jy * f.view_projection[c * 4 + 3];
    f.transparent_view_projection[c * 4 + 0] += jx * f.transparent_view_projection[c * 4 + 3];
    f.transparent_view_projection[c * 4 + 1] += jy * f.transparent_view_projection[c * 4 + 3];
  }
}

void compute_clip_from_world(const ShowroomFrame& f, const cars::bake::CarInfo& info, const drive::Vehicle& v,
                             float out[16]) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(info, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += f.view_projection[k * 4 + r] * world_to_model[c * 4 + k];
      out[c * 4 + r] = sum;
    }
  }
}

// The rig's frame: the car's pose and motion in, the view out (in the
// car's model frame, where the showroom draws).
ShowroomFrame rig_frame(drive::DriveCamera& rig, const cars::bake::CarInfo& info, const drive::Vehicle& v,
                        const drive::Ground* ground, carview::RigAccelState& accel, double dt, float aspect,
                        bool reverse_z, bool* interior, double* com_distance = nullptr,
                        drive::CameraView* out_view = nullptr) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(info, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  drive::CameraInput in;
  // The car frame is the model frame with z negated.
  for (int k = 0; k < 16; ++k) in.car_to_world[k] = model_to_world[k] * (k >= 8 && k < 12 ? -1.0 : 1.0);
  const drive::Vec3 vel = v.velocity();
  in.speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
  const drive::Vec3 fwd = drive::rotate(v.orientation(), {0, 0, 1});
  in.heading = std::atan2(fwd.x, fwd.z);
  in.yaw_rate = v.angular_velocity().y;
  in.pitch_degrees = std::asin(std::clamp(fwd.y, -1.0, 1.0)) * 180 / 3.14159265358979323846;
  // The g layers' inputs: the body's acceleration over the frame along its
  // forward and right axes, gravity left out (SPEC-CAMERAS, Inputs).
  accel.sample(vel, v.orientation(), dt, in.g_longitudinal, in.g_lateral);
  int down = 0;
  const double c = std::cos(info.pitch), s = std::sin(info.pitch);
  double slip_long_sum = 0, slip_lat_sum = 0;
  for (int i = 0; i < 4; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    down += w.contact ? 1 : 0;
    const double y = w.centre.y - v.params().tyre_radius[size_t(i)] - info.lift;
    in.wheel_ground_y[size_t(i)] = c * y + s * w.centre.z;
    in.surface_bump[size_t(i)] = w.contact ? w.compression : 0.0;
    slip_long_sum += w.slip_ratio;
    slip_lat_sum += w.slip_angle;
  }
  in.contact = down / 4.0;
  in.slip_longitudinal = slip_long_sum * 0.25;
  in.slip_lateral = slip_lat_sum * 0.25;
  in.suspension_bias_longitudinal =
      0.5 * (v.wheels()[0].compression + v.wheels()[1].compression) -
      0.5 * (v.wheels()[2].compression + v.wheels()[3].compression);
  if (v.body_contact_force() > 0.0) {
    const auto& bc = v.strongest_body_contact();
    const double mag = bc.force / std::max(1.0, v.params().mass);
    const drive::Vec3 right = drive::rotate(v.orientation(), {1, 0, 0});
    in.world_collision_longitudinal = mag * (bc.normal.x * fwd.x + bc.normal.y * fwd.y + bc.normal.z * fwd.z);
    in.world_collision_lateral = mag * (bc.normal.x * right.x + bc.normal.y * right.y + bc.normal.z * right.z);
  }
  if (ground)
    in.ground_below = [ground](double x, double y, double z, double& gy) {
      drive::Hit hit;
      if (!ground->raycast({x, y, z}, {0, -1, 0}, 60, hit)) return false;
      gy = y - hit.distance;
      return true;
    };
  const drive::CameraView view = rig.update(dt, in);
  if (out_view) *out_view = view;
  if (interior) *interior = view.interior;
  if (com_distance) {
    const drive::Vec3 com = v.centre_of_mass();
    *com_distance = std::hypot(view.eye[0] - com.x, view.eye[1] - com.y, view.eye[2] - com.z);
  }
  const V3 eye{float(view.eye[0]), float(view.eye[1]), float(view.eye[2])};
  const V3 at{float(view.eye[0] + view.forward[0]), float(view.eye[1] + view.forward[1]),
              float(view.eye[2] + view.forward[2])};
  const V3 up{float(view.up[0]), float(view.up[1]), float(view.up[2])};
  return look_frame(transform(world_to_model, eye, 1), transform(world_to_model, at, 1),
                    norm(transform(world_to_model, up, 0)), aspect, false, reverse_z, float(view.fov_degrees),
                    float(view.near_clip));
}

// The brake lamps' glow (an interim: FORMAT-CARS leaves the lit state to
// the effect): on while a front wheel gets the brake or the ABS works;
// SHIFT_BRAKE_GLOW (0 to 1) holds it for a render.
float brake_glow(const drive::Vehicle& v) {
  static const char* held = std::getenv("SHIFT_BRAKE_GLOW");
  if (held) return float(std::atof(held));
  const auto& w = v.wheels();
  return w[0].brake > 0.02 || w[1].brake > 0.02 || v.abs_active() ? 1.0f : 0.0f;
}

void wheel_poses(const drive::Vehicle& v, WheelPose poses[4]) {
  for (int i = 0; i < 4; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    poses[i].centre[0] = float(w.centre.x);
    poses[i].centre[1] = float(w.centre.y);
    poses[i].centre[2] = float(w.centre.z);
    poses[i].steer = float(w.steer);
    poses[i].spin = float(w.spin);
    poses[i].omega = float(w.omega);
  }
}

const char* kDriveRuns[][2] = {
    {"rest", "0 0 0 0\n"},
    {"launch", "0 0 1 0\n"},
    {"slalom", "0 0 0.6 0\n2 0.8 0.5 0\n3 -0.8 0.5 0\n4 0.8 0.5 0\n5 -0.8 0.5 0\n6 0 0 0.6\n"},
    {"circle", "0 0 0 0\n1 0.5 0.35 0\n"},
    {"drift", "0 0 1 0\n1.8 0.95 1 0\n2.6 0.85 1 0\n3.4 -0.7 1 0\n4.2 0.85 1 0\n"},
    {"cruise", "0 0 0 0\n"},  // input from world::RoadFollower (needs --world)
};

// Flat colours per surface class (ours; the textured world's materials
// without a texture, and --flat-world): road, terrain, water, other, prop.
const std::array<float, 3> kSurfaceColours[] = {
    {0.035f, 0.035f, 0.04f}, {0.11f, 0.13f, 0.05f}, {0.03f, 0.07f, 0.12f}, {0.30f, 0.28f, 0.25f}, {0.20f, 0.17f, 0.12f}};

// The streamed mesh as showroom scenery, relative to `origin` (x, 0, z).
WorldTriangles scenery(const world::Mesh& mesh, double origin_x, double origin_z) {
  WorldTriangles w;
  w.colours.assign(std::begin(kSurfaceColours), std::end(kSurfaceColours));
  w.positions.reserve(mesh.positions.size());
  for (const auto& p : mesh.positions)
    w.positions.push_back({float(p[0] - origin_x), p[1], float(p[2] - origin_z)});
  w.group.reserve(mesh.triangles());
  for (auto s : mesh.surface) w.group.push_back(uint8_t(s));
  if (mesh.has_render_data()) {
    w.normals = mesh.normals;
    w.uv0 = mesh.uv0;
    w.uv1 = mesh.uv1;
    w.uv2 = mesh.uv2;
    w.vertex_colours = mesh.colours;
    w.material = mesh.material;
    // Prop origins in the scenery's frame, like the positions.
    w.instance.reserve(mesh.instance.size());
    for (const auto& p : mesh.instance) w.instance.push_back({float(p[0] - origin_x), p[1], float(p[2] - origin_z)});
    for (const auto& m : mesh.materials) {
      WorldTriangles::Material wm;
      wm.texture = m.albedo;
      wm.blend = uint8_t(m.blend);
      wm.far = m.far;
      wm.decal = uint8_t(world::decal_effect(m.effect) ? 1 : 0);
      wm.layers = uint8_t(m.layers);
      wm.layer_b = m.layer_b;
      wm.splat = m.splat;
      wm.light_map = m.light_map;
      wm.normal_map = m.normal_map;
      wm.ao_map = m.ao_map;
      wm.layer_c = m.layer_c;
      wm.spec_map = m.spec_map;
      wm.tiling = m.tiling;
      wm.tiling_c = m.tiling_c;
      wm.decal_noise_c4 = m.decal_noise_c4;
      wm.emissive_force = m.emissive_force;
      wm.is_water = m.is_water;
      wm.anim_flag = m.anim_flag;
      wm.is_grass = m.is_grass;
      wm.is_road = m.is_road;
      wm.fade = m.fade;
      w.materials.push_back(wm);
    }
  }
  return w;
}

// The world textures decoded so far, by texture record. Only the gathers'
// Prepare touches it, and gathers run one at a time.
struct WorldTextureLoader {
  std::set<uint32_t> failed;
  std::set<uint32_t> cutout;  // textures whose base level has alpha under one half
  size_t decoded = 0;
  // Decodes the textures `w` names that are not already resident in `showroom`;
  // resident records are pinned in `pin_records` until the staged world is
  // consumed or dropped. A texture that does not decode leaves its materials flat.
  void load(world::World& colorado, WorldTriangles& w, Showroom& showroom,
            std::vector<uint32_t>& pin_records) {
    std::unordered_set<uint32_t> batch_seen;
    for (auto& m : w.materials)
      for (uint32_t* slot :
           {&m.texture, &m.layer_b, &m.splat, &m.light_map, &m.normal_map, &m.ao_map, &m.layer_c, &m.spec_map})
        load_one(colorado, w, showroom, batch_seen, pin_records, *slot);
  }
  void load_one(world::World& colorado, WorldTriangles& w, Showroom& showroom,
                std::unordered_set<uint32_t>& batch_seen, std::vector<uint32_t>& pin_records,
                uint32_t& record) {
    if (record == kNoWorldTexture) return;
    if (failed.count(record)) {
      record = kNoWorldTexture;
      return;
    }
    if (!batch_seen.insert(record).second) return;
    if (showroom.reserve_world_texture(record)) {
      pin_records.push_back(record);
      return;
    }
    fh1::WorldTexture t;
    std::string e;
    if (!colorado.read_texture(record, t, &e)) {
      std::fprintf(stderr, "carview: texture record %u: %s\n", record, e.c_str());
      failed.insert(record);
      record = kNoWorldTexture;
      return;
    }
    if (fh1::world_texture_has_cutout(t)) cutout.insert(record);
    auto d = std::make_shared<WorldTextureData>();
    d->record = record;
    d->srgb = t.format.gamma;
    char name[40];
    std::snprintf(name, sizeof name, "colorado:_0x%08x", colorado.textures()[record].id);
    d->texture.name = name;
    d->texture.format = cars::bake::TextureFormat(uint32_t(t.blocks.format));
    d->texture.swizzle = t.blocks.swizzle;
    for (auto& l : t.blocks.levels) d->texture.levels.push_back({l.width, l.height, std::move(l.blocks)});
    carview::world_texture_hook(record, d->srgb, d->texture);  // mods' replacement textures (mods_page.h)
    w.textures.push_back(std::move(d));
    ++decoded;
  }
};

// WORLD-MATERIALS' sunDir (daytime free-roam frame), in the game frame.
constexpr float kSunGame[3] = {0.3848f, 0.8754f, 0.2926f};

// A direction through the rotation part of a column-major matrix, normalised.
void rotate_direction(const float m[16], const float v[3], float out[3]) {
  double len = 0;
  for (int r = 0; r < 3; ++r) {
    out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2];
    len += double(out[r]) * out[r];
  }
  len = std::sqrt(len);
  if (len > 0)
    for (int r = 0; r < 3; ++r) out[r] = float(out[r] / len);
}

// The scenery's transform: world to model, after the origin's translation.
void scenery_transform(const cars::bake::CarInfo& car, const drive::Vehicle& v, double origin_x, double origin_z,
                       float out[16]) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(car, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  float shift[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(origin_x), 0, float(origin_z), 1};
  multiply4(world_to_model, shift, out);
}

}  // namespace

// carview --drive: one car with the engine's placeholder physics
// (libs/drive), on flat ground or, with --world, in Colorado: the render
// models and props streamed around the car (libs/world), drawn in flat
// colours per surface class and driven on as the stand-in ground. Headless,
// it runs a built-in or scripted input and writes chase-camera frames
// every --every seconds; windowed, it is driven with the keyboard (arrows
// or WASD, space for the handbrake, R to go back to the start) or a
// gamepad (left stick, triggers, A), with speed, gear and rpm in the title.
//
//   carview --drive --bake <dir> --car <media> [--run rest|launch|slalom|circle|cruise
//           | --script file] [--speed kmh] [--view chase|front] [--seconds N] [--every S]
//           [--world <colorado dir> [--cache file] [--near x z] [--spawn x y z heading]
//            [--render-ground] [--flat-world] [--no-props] [--no-far] [--no-trees] [--world-reference freeroam.json]]
//           [--headless --out prefix.png [--hud]] [--size WxH]
//
// With --world the car drives on Colorado's collision (COLLISION.md `.fiz`
// tiles; --render-ground: the earlier render-triangle stand-in) and starts
// at COLLISION.md's measured free-roam spawn by the festival, or on the road
// triangle nearest --near (x, z in the game frame, the file frame with z
// negated), or at --spawn, dropped onto the ground. --run cruise drives
// itself along the road at --speed (default 40 km/h).
//
// The scenery is drawn with its own textures (WORLD-MATERIALS' first-pass
// recipe: slot 0 lit by the frame's sun and sky); --flat-world draws the
// earlier flat colours per surface class, and --no-props leaves the
// placed props out. The textured world also draws the far ring (the
// MIDDIST and UberLOD terrain within 3 km; --no-far leaves it out), the
// tree sprites within kTreeRadius (--no-trees leaves them out) and a sky
// gradient. --world-reference (headless)
// gathers around a world reference's camera and renders that one frame
// without the car at its matrix and size: <out> and, beside it, <out stem>_vs.png
// with the reference image on the left.
// How far out the tree sprites are gathered (ours; each record's own far
// distance also applies).
constexpr double kTreeRadius = 2000;
// Popping (ours): the drive's gather radii, the regather step and the swap's
// lag behind it, the props' LOD distance and cross-fade bands, and how long
// a regathered scenery dithers in over the old one.
constexpr double kModelRadius = 900, kPropRadius = 600, kRegather = 200, kSwapLag = 50;
// The low preset's gathers (ours): the M2 UHD 620 gate (GPU p95 under
// 16 ms at 1280x720); props still fade out before every gather's edge.
constexpr double kLowModelRadius = 600, kLowPropRadius = 500;
constexpr double kPropLod0 = 80, kPropLodBand = 20, kPropEdgeBand = 40, kSwapFadeSeconds = 0.5;
// The scene's scale into the filmic curve (ours: the track's filmicTone
// exposure, 0.31 on Colorado, applies to a scene the game lights more
// brightly than ours; set so the free-roam reference frame's mean is met).
constexpr double kPostExposure = 0.8;
// Ours: the warm grade after the curve (PostSettings::balance), fitted to
// the hill reference frame (hill-2244-707): the game is warmer than ours.
constexpr float kPostBalance[3] = {1.10f, 0.88f, 0.88f};

// The post chain's constants from the track's TrackSettings.xml
// (SHOWROOM_LIGHTING, Post chain: filmicTone's day curve, DesaturateAmount,
// DesatLuminosity, bloom cutoff and scale). Without the file PostSettings'
// defaults (Colorado's values) stay. The exposure is ours (see the caller).
void read_post_settings(const std::filesystem::path& world_dir, PostSettings& s) {
  std::ifstream in(world_dir / "TrackSettings.xml", std::ios::binary);
  if (!in) return;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  audio::XmlNode root;
  if (!audio::parse_xml(text, root, nullptr)) return;
  if (const auto* f = root.child("filmicTone")) {
    s.white = float(f->number("white", s.white));
    const char* keys[6] = {"shoulderStrength", "linearStrength", "linearAngle", "toeStrength", "toeNumerator",
                           "toeDenominator"};
    for (int i = 0; i < 6; ++i) s.hable[i] = float(f->number(keys[i], s.hable[i]));
  }
  if (const auto* d = root.child("DesaturateAmount")) s.desaturate = float(d->number("value", s.desaturate));
  if (const auto* l = root.child("DesatLuminosity")) {
    s.luminance[0] = float(l->number("r", s.luminance[0]));
    s.luminance[1] = float(l->number("g", s.luminance[1]));
    s.luminance[2] = float(l->number("b", s.luminance[2]));
  }
  if (const auto* c = root.find("bloom/cutoff")) s.bloom_cutoff = float(c->number("value", s.bloom_cutoff));
  if (const auto* c = root.find("bloom/scale")) s.bloom_scale = float(c->number("value", s.bloom_scale));
}

// The track's shadow light (TrackSettings: ShadowLightDirection, the
// direction the light travels in the game frame, used when
// OverrideShadowLightDirection is set). False without it.
bool read_shadow_direction(const std::filesystem::path& world_dir, float travel[3]) {
  std::ifstream in(world_dir / "TrackSettings.xml", std::ios::binary);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  audio::XmlNode root;
  if (!audio::parse_xml(text, root, nullptr)) return false;
  const auto* on = root.child("OverrideShadowLightDirection");
  const auto* d = root.child("ShadowLightDirection");
  if (!on || !d || on->number("value", 0) < 0.5) return false;
  travel[0] = float(d->number("x", 0));
  travel[1] = float(d->number("y", -1));
  travel[2] = float(d->number("z", 0));
  return true;
}

// The free-roam progress to and from the save (drive/save.h): barns by
// name, an unknown status read as not spawned.
// The cars the bake has, by Data_Car id, from each physics file's
// "car <id> <media>" line.
std::map<int64_t, std::string> baked_car_ids(const std::filesystem::path& bake_dir) {
  std::map<int64_t, std::string> out;
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(bake_dir / "cars", ec)) {
    if (!f.path().string().ends_with(".physics.txt")) continue;
    std::ifstream in(f.path());
    for (std::string line; std::getline(in, line);)
      if (line.rfind("car ", 0) == 0) {
        std::istringstream ls(line.substr(4));
        int64_t id = 0;
        std::string media;
        if (ls >> id >> media) out[id] = media;
        break;
      }
  }
  return out;
}

void progress_from_save(const drive::DriveSave& s, const freeroam::Collectibles& data, freeroam::Progress& p) {
  p.total_distance = s.total_distance;
  p.signs_found.clear();
  for (const auto& id : s.signs_found)
    for (const auto& sign : data.signs)
      if (sign.id == id) p.signs_found.insert(id);
  p.barns.assign(data.barns.size(), {});
  for (const auto& b : s.barns)
    for (size_t i = 0; i < data.barns.size(); ++i)
      if (data.barns[i].name == b.name) {
        const bool known = b.status == freeroam::kBarnFound || b.status == freeroam::kBarnNotSpawned ||
                           b.status == freeroam::kBarnRumour;
        p.barns[i] = {known ? b.status : int(freeroam::kBarnNotSpawned), b.rumour_at, b.found_at};
      }
  p.garage = s.garage;
}

void progress_to_save(const freeroam::Collectibles& data, const freeroam::Progress& p, drive::DriveSave& s) {
  s.total_distance = p.total_distance;
  s.signs_found.assign(p.signs_found.begin(), p.signs_found.end());
  s.barns.clear();
  for (size_t i = 0; i < data.barns.size() && i < p.barns.size(); ++i)
    if (p.barns[i].status != freeroam::kBarnNotSpawned)
      s.barns.push_back({data.barns[i].name, p.barns[i].status, p.barns[i].rumour_at, p.barns[i].found_at});
  s.garage = p.garage;
}

// "IDS_Barnfind_Car_BuggatiEB110SS" -> "BuggatiEB110SS" (the string table
// is not read yet).
std::string barn_car_label(const freeroam::Barn& b) {
  const std::string prefix = "IDS_Barnfind_Car_";
  if (b.car_name_key.rfind(prefix, 0) == 0) return b.car_name_key.substr(prefix.size());
  return b.car_name_key.empty() ? "car " + std::to_string(b.car_id) : b.car_name_key;
}

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
               "  other modes: carview --bake <dir> --car <media> (showroom), carview --game-root <dir> (disc viewer),\n"
               "           carview --world ... (world viewer), carview --xds-png ...\n");
}

int run_drive(int argc, char** argv) {
  const auto drive_start_tp = std::chrono::steady_clock::now();
  carview::bench::StartupMetrics bench_startup{};
  // The post chain (on unless --no-post); --exposure overrides ours,
  // --bloom / --no-bloom the preset's choice.
  bool post_on = true;
  int bloom_flag = -1;
  int motion_blur_flag = -1;
  int dof_flag = -1;
  double exposure_given = 0;
  bool low_preset = false;
  // The sun shadow (ours): --shadows off, low (1024 texels over 80 m, one
  // filtered compare) or high (2048 over 120 m, four); the preset's by
  // default (low: low, otherwise high).
  std::string shadows_flag;
  std::filesystem::path bake_dir, script_path, world_dir, cache_path;
  double near_x = world::kDumpCameraX, near_z = world::kDumpCameraZ, near_heading = world::kDumpCameraHeading;
  double cruise_kmh = 40, spawn[4] = {0, 0, 0, 0};
  // The disc's root (media/ inside): from --game, else three folders up
  // from --world (media/tracks/<world>). A world staged elsewhere (the
  // Rally package's ColoradoDirt) needs --game.
  std::filesystem::path game_root_given;
  bool near_given = false, have_spawn = false, fast_travel = false, render_ground = false, async_stream = false,
       flat_world = false, no_props = false, no_far = false, no_trees = false;
  bool bench = false;     // headless: render every frame, timed, no images
  carview::bench::Scenario bench_scenario = carview::bench::Scenario::kCustom;
  bool bench_scenario_set = false;
  std::filesystem::path bench_json_path, bench_csv_path;
  bool bench_full_fx = false;
  bool bench_audio = false;
  double bench_warmup_sec = 0.0;
  bool bench_warmup_explicit = false;
  std::string preset_name = "high";
  bool cruise_kmh_explicit = false;
  uint32_t window_frames_limit = 0;  // --window-frames N: the window closes after N presented frames (soaks, perf)
  carview::FrameStats frame_stats;   // presented-frame times of the window
  uint32_t msaa = 4;
  std::filesystem::path world_reference_path;
  std::string car, out, run = "slalom", view = "chase";
  uint32_t width = 960, height = 540;
  bool size_given = false;
  double seconds = 6, every = 1;
  bool headless = false;
  bool hud = false;  // headless: draw the HUD into the frames (windowed: always, H toggles)
  // The drive's save (drive/save.h): the window's default is our own
  // user-data folder (SDL's pref path, shift-engine/drive); --save names
  // the file (headless too), --no-save turns it off. --menu-png writes the
  // HUD with the pause menu open and exits (a headless check of the menu).
  std::filesystem::path save_path, menu_png;
  // --import-profile: the player's profile export (FORMAT-PROFILE) as the
  // save; the drive starts at the festival and the first save replaces the
  // engine's save (the old one kept beside it as .pre-import).
  std::filesystem::path import_dir;
  bool import_again = false;  // --import-profile-again: replace a save that came from an import
  bool no_save = false;
  const bool validation = std::getenv("SHIFT_VALIDATION") != nullptr;
  // The engine sound (SPEC-ENGINE-SOUND), baked by soundbake.
  std::filesystem::path sound_dir = "out/sound", audio_out;
  bool sound = true, sound_given = false;
  bool radio_on = true;
  double radio_press_every = 0;
  carview::AudioConfig audio_cfg;
  // --event: an event's race (SPEC-EVENTS, SPEC-RACE-AI).
  std::string race_event;
  carview::RaceView::Options race_options;
  int race_auto = -1;  // -1: on headless, off in the window
  // The play folder (tools/setup.py; README, Play the drive).
  std::filesystem::path play_dir;
  // Free-roam traffic (SPEC-TRAFFIC): on in the window with the world, off
  // headless unless --traffic; --hour sets the time of day it follows (the
  // world's lighting does not change with it), T / Shift+T in the window.
  int traffic_flag = -1;  // -1: the default
  double hour = 12;
  double day_length_scale = 1.0;
  bool hour_explicit = false;
  uint64_t traffic_seed = 1;
  // --contacts-log (ours, debugging): every hard body contact against the
  // collision, and per gather the walls nothing drawn stands near.
  std::filesystem::path contacts_log_path;
  bool texture_census = false;
  // Free-roam collectibles (SPEC-FREEROAM-COLLECTIBLES): on unless
  // --no-collectibles or --bench (a smash regathers); --reveal shows the
  // unfound ones on the map; --distance adds metres to the total driven;
  // --map-png writes the pause map and the minimap at the end.
  bool collectibles_on = true, reveal = false, treasure_map_setting = false;
  bool has_cli_gps = false;
  double cli_gps_x = 0.0, cli_gps_z = 0.0;
  std::string cli_pr_stunt, cli_outpost_hub;
  bool demo_speed_trap = false, demo_h2h = false;
  bool demo_rally_stage = false, demo_rally_service = false, demo_club = false;
  std::filesystem::path pause_map_png;
  double cli_map_zoom = 4.5;
  double seed_distance = 0;
  std::filesystem::path map_png, career_png;  // --career-png: the career page's windows, then exit
  int gather_route = 0;  // --route: the placements' route (-1: every one)
  bool gather_route_given = false;
  int camera_mode = 0;   // --camera: the drive camera to start in (0-4, SPEC-CAMERAS' cycle order)
  std::string language = "EN";
  // Reversed depth with an infinite far plane (ours; --no-reverse-z: the
  // 0.25 m to 16 km projection).
  bool reverse_z = true;
  bool backface_cull = (std::getenv("SHIFT_NO_CULL") == nullptr);
  bool z_prepass = (std::getenv("SHIFT_Z_PREPASS") != nullptr);
  carview::modern::GraphicsSettings cli_graphics;
  uint32_t cli_hdr_pattern = 0;
  float cli_hdr_pattern_nits = 1000.0f;
  bool cli_perf_overlay = false;
  bool cli_frame_inspector = false;
  carview::InspectorView cli_inspector_view = carview::InspectorView::kColor;
  // The player's settings (settings.h) go in before the command line, in
  // the window only; --no-settings skips them, --save-settings keeps this
  // run's options for the next.
  std::vector<std::string> args;
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
  bool photo_through_walls_cli = false;
  bool photo_wider_limits_cli = false;
  std::filesystem::path demo_photo_png;
  std::filesystem::path demo_replay_png;
  std::filesystem::path demo_achievements_png;
  std::filesystem::path demo_touch_hud_png;
  carview::TouchControls touch_controls;
  std::string android_gpu_driver = "auto";
  bool save_settings = false;
  bool no_title = false;
  bool skip_intro_movies = false;
  bool start_in_single_player = false;
  bool skip_vo_with_a = true;
  bool story_intro_cli = false;
  std::filesystem::path cli_fmv_path;
  std::filesystem::path mods_root;
  bool mods_off = false;
  // Slice 13: native mods opt-in, save sync folder, device and profile.
  bool mods_native = false;
  std::filesystem::path sync_folder;
  std::string device_name, profile_name;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string a = args[i];
    auto next = [&]() -> const char* { return i + 1 < args.size() ? args[++i].c_str() : ""; };
    if (a == "--drive" || a == "--no-settings") continue;
    if (a == "--bench-scenario") {
      const std::string sc_name = next();
      if (!carview::bench::parse_scenario(sc_name, bench_scenario)) {
        std::fprintf(stderr, "carview: unknown --bench-scenario '%s'\n", sc_name.c_str());
        return 2;
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
    if (a == "--save-settings") save_settings = true;
    else if (a == "--no-title") no_title = true;
    else if (a == "--skip-intro-movies") skip_intro_movies = true;
    else if (a == "--no-skip-intro-movies") skip_intro_movies = false;
    else if (a == "--start-in-single-player") start_in_single_player = true;
    else if (a == "--no-start-in-single-player") start_in_single_player = false;
    else if (a == "--skip-vo-with-a") skip_vo_with_a = true;
    else if (a == "--no-skip-vo-with-a") skip_vo_with_a = false;
    else if (a == "--story-intro") story_intro_cli = true;
    else if (a == "--fmv") cli_fmv_path = next();
    else if (a == "--mods") mods_root = next(), mods_off = false;
    else if (a == "--no-mods") mods_off = true;
    else if (a == "--mods-native") mods_native = true;
    else if (a == "--sync-folder") sync_folder = next();
    else if (a == "--device-name") device_name = next();
    else if (a == "--profile") profile_name = next();
    else if (a == "--headless") headless = true;
    else if (a == "--bake") bake_dir = next();
    else if (a == "--car") car = next();
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
        if (shadows_flag.empty()) shadows_flag = pspec.shadows;
        if (bloom_flag < 0) bloom_flag = pspec.bloom ? 1 : 0;
      } else {
        msaa = p == "low" ? 1 : 4;
        low_preset = p == "low";
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
        return 2;
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
        return 2;
      }
    } else if (a == "--ai-mode") {
      if (!race::parse_ai_mode(next(), race_options.ai_mode)) {
        std::fprintf(stderr, "carview: --ai-mode stock|fair|hard\n");
        return 2;
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
      if (std::sscanf(next(), "%ux%u", &width, &height) != 2 || !width || !height) return 2;
      size_given = true;
    } else if (carview::modern::apply_graphics_option(cli_graphics, args, i)) {
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
      return 0;
    } else {
      std::fprintf(stderr, "carview: unknown option %s\n", a.c_str());
      drive_usage();
      return 2;
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
      return 1;
    }
    std::printf("settings: saved %s\n", file.string().c_str());
  }
  if (!menu_png.empty()) {
    {
      const char* g = std::getenv("SHIFT_GAME_ROOT");
      load_ui_fonts(!game_root_given.empty() ? game_root_given : std::filesystem::path(g ? g : ""));
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
    if (!write_menu_png(st, menu_png.string(), has_board ? &sample_board : nullptr)) return 1;
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
      auto hud_png = menu_png;
      hud_png.replace_filename(menu_png.stem().string() + "_hud.png");
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
      auto race_png = menu_png;
      race_png.replace_filename(menu_png.stem().string() + "_race.png");
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
      auto res_png = menu_png;
      res_png.replace_filename(menu_png.stem().string() + "_results" + std::to_string(page) + ".png");
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
      auto out_p = menu_png;
      out_p.replace_filename(menu_png.stem().string() + suffix + ".png");
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
    std::printf("menu: 1280x720 to %s\n", menu_png.string().c_str());
    return 0;
  }
  const bool car_given = !car.empty();
  // The play test's defaults for a window: the bake in out/bake (or
  // out/bake1), the R34, Colorado from SHIFT_GAME_ROOT or the sibling
  // horizon1-recomp checkout, at the free-roam spawn, 1600 x 900.
  if (!headless && !no_save && save_path.empty()) {
    if (char* pref = SDL_GetPrefPath("shift-engine", "drive")) {
      save_path = std::filesystem::path(pref) / "save.txt";
      SDL_free(pref);
    }
  }
  if (no_save) save_path.clear();
  bool slice12_modded_save = false;
  for (const auto& a : args) {
    if (a == "--trainer-cheats" || a == "--festival-pacing" || a == "--hidden-cars") slice12_modded_save = true;
  }
  // Mods & Save Ecosystem (mods_page.h, save_sync_ui.h): resolve mods first
  // to know whether an isolated modded save is active (isolated modded saves
  // never sync), boot profiles / Save Sync / rolling backups, then apply
  // modded-save isolation before reading save.txt.
  carview::ModsSession mods_session;
  carview::SaveSession save_session;
  {
    const char* sr_env = std::getenv("SHIFT_STATE_ROOT");
    const std::filesystem::path sr = sr_env ? std::filesystem::path(sr_env) : std::filesystem::path{};
    std::error_code ec;
    const bool has_v4 = !sr.empty() && std::filesystem::is_regular_file(sr / "title-update-v4" / "media.zip", ec);
    const auto build = has_v4 ? cars::modmgr::GameBuild::kV4 : cars::modmgr::GameBuild::kBase;
    if (!mods_off) {
      if (mods_root.empty() && !headless) mods_root = carview::default_mods_root();
      mods_session.boot(mods_root, {}, build, sr);
    }
    const bool isolated_mod = slice12_modded_save || (!mods_off && !mods_session.resolution().save_profile.empty());
    if (!save_path.empty()) {
      carview::SaveSession::BootOptions sb;
      sb.user_dir = carview::default_user_dir();
      if (sb.user_dir.empty()) sb.user_dir = save_path.parent_path().parent_path();
      sb.default_save_dir = save_path.parent_path();
      sb.sync_folder = sync_folder;
      sb.device_name = device_name;
      sb.profile_override = profile_name;
      sb.v4_available = has_v4;
      sb.isolated_mod_save = isolated_mod;
      sb.achievements_path = carview::default_achievements_path();
      sb.replays_dir = carview::default_replays_dir();
      const auto prof_dir = save_session.boot(sb);
      save_path = prof_dir / "save.txt";
      for (const auto& line : save_session.log) std::printf("save: %s\n", line.c_str());
      save_session.log.clear();
    }
    if (!mods_off) {
      const auto dir = mods_session.boot(mods_root, save_path.empty() ? std::filesystem::path{} : save_path.parent_path(),
                                         build, sr);
      if (!save_path.empty()) save_path = dir / "save.txt";
      for (const auto& line : mods_session.log) std::printf("mod: %s\n", line.c_str());
      mods_session.log.clear();
    }
  }
  if (!save_path.empty() && slice12_modded_save && !mods_session.isolation().isolated) {
    save_path = drive::resolve_isolated_save_path(save_path, true);
  }
  // The save: its car when none was given (and it is baked), and where it
  // stood when no spawn was given and the car is the same.
  drive::DriveSave saved;
  bool have_saved = false;
  if (!save_path.empty()) {
    std::string e;
    have_saved = drive::read_save(save_path, saved, &e);
    if (!have_saved && !e.empty()) std::fprintf(stderr, "carview: note: save ignored: %s\n", e.c_str());
  }
  // The play folder tools/setup.py makes: play.txt ("game <root>", the
  // extracted disc) with bake/, sound/ and world/colorado.index beside it.
  // --play names it; otherwise a window looks in SHIFT_PLAY_DIR, the
  // executable's folder, then the user-data folder (SDL's pref path,
  // shift-engine/play). It fills what the arguments left out (headless
  // too when no --bake is given).
  if ((!headless || bake_dir.empty()) && play_dir.empty()) {
    std::vector<std::filesystem::path> where;
    if (const char* p = std::getenv("SHIFT_PLAY_DIR")) where.emplace_back(p);
    if (const char* b = SDL_GetBasePath()) where.emplace_back(b);
#ifdef SDL_PLATFORM_ANDROID
    // The app's external files folder (Android/data/<app>/files/play).
    if (const char* x = SDL_GetAndroidExternalStoragePath()) where.emplace_back(std::filesystem::path(x) / "play");
#endif
    if (char* pref = SDL_GetPrefPath("shift-engine", "play")) {
      where.emplace_back(pref);
      SDL_free(pref);
    }
    std::error_code ec;
    for (const auto& d : where)
      if (!d.empty() && std::filesystem::is_regular_file(d / "play.txt", ec)) {
        play_dir = d;
        break;
      }
  }
  if (!play_dir.empty()) {
    std::ifstream in(play_dir / "play.txt");
    if (!in) {
      std::fprintf(stderr, "carview: no play.txt in %s (run tools/setup.py)\n", play_dir.string().c_str());
      return 2;
    }
    std::filesystem::path game;
    for (std::string line; std::getline(in, line);) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("game ", 0) == 0) game = std::filesystem::path(line.substr(5));
    }
    if (!game.empty() && game.is_relative()) game = play_dir / game;
    if (bake_dir.empty()) bake_dir = play_dir / "bake";
    if (!sound_given) sound_dir = play_dir / "sound";
    if (cache_path.empty()) cache_path = play_dir / "world" / "colorado.index";
    std::error_code ec;
    if (world_dir.empty() && !game.empty())
      world_dir = (std::filesystem::is_directory(game / "media", ec) ? game / "media" : game) / "tracks" / "colorado";
    std::printf("play: %s\n", play_dir.string().c_str());
  }
  if (!headless) {
    std::error_code ec;
    if (bake_dir.empty())
      for (const char* d : {"out/bake", "out/bake1"})
        if (std::filesystem::is_directory(std::filesystem::path(d) / "cars", ec)) {
          bake_dir = d;
          break;
        }
    if (car.empty() && have_saved) {
      std::error_code e2;
      if (std::filesystem::exists(bake_dir / "cars" / (lower_ascii(saved.car) + ".shiftbake"), e2)) car = saved.car;
    }
    if (car.empty()) car = "NIS_SkylineGTRVSpecII_02";
    if (world_dir.empty()) {
      std::vector<std::filesystem::path> roots;
      if (const char* g = std::getenv("SHIFT_GAME_ROOT")) roots.emplace_back(g);
      roots.emplace_back("../horizon1-recomp/.local/game/base");
      for (const auto& r : roots) {
        const auto d = (std::filesystem::is_directory(r / "media", ec) ? r / "media" : r) / "tracks" / "colorado";
        if (std::filesystem::is_directory(d, ec)) {
          world_dir = d;
          break;
        }
      }
    }
    if (!size_given) width = 1600, height = 900;
    std::printf("drive: bake %s, car %s, world %s\n", bake_dir.empty() ? "(none found)" : bake_dir.string().c_str(),
                car.c_str(), world_dir.empty() ? "(none: flat ground)" : world_dir.string().c_str());
  }
  // Mods (cars/mod_manager.h): the window's from the user folder's mods/, a
  // headless run's only from --mods; --no-mods turns them off.
  if (!mods_off) {
    const std::filesystem::path mods_game_root =
        !game_root_given.empty() ? game_root_given
                                 : (!world_dir.empty() ? world_dir.parent_path().parent_path().parent_path()
                                                       : std::filesystem::path{});
    mods_session.activate(bake_dir, sound_dir, mods_game_root);
    mods_session.load_plugins(mods_native);
    carview::set_world_texture_session(&mods_session);
    for (const auto& line : mods_session.log) std::printf("mod: %s\n", line.c_str());
    mods_session.log.clear();
  }
  // Colorado's own features (collectibles, the map, traffic) stay off in
  // another world (ColoradoDirt: SPEC-V4-CONTENT, no traffic on a stage).
  auto colorado_world = [&]() { return world_dir.empty() || world::model_prefix(world_dir) == "coloradoout"; };
  auto game_root_of = [&]() -> std::filesystem::path {
    return !game_root_given.empty() ? game_root_given : world_dir.parent_path().parent_path().parent_path();
  };
  const std::filesystem::path state_root = [] {
    const char* s = std::getenv("SHIFT_STATE_ROOT");
    return s ? std::filesystem::path(s) : std::filesystem::path{};
  }();
  // The parts database (the merged one: SHIFT_STATE_ROOT's packages when
  // set), opened in the background; opened early so load_ui_fonts and garage
  // setup overlap with the SQLite merge.
  std::shared_future<std::shared_ptr<cars::PartsDb>> parts_db;
  if (!world_dir.empty()) {
    const auto game_root = game_root_of();
    const std::string sr = state_root.string();
    parts_db = std::async(std::launch::async, [game_root, sr]() -> std::shared_ptr<cars::PartsDb> {
                 cars::Install install;
                 auto db = std::make_shared<cars::PartsDb>();
                 std::string why;
                 if (!cars::discover_install(game_root, sr, "", install, &why) || !db->open(install, &why)) {
                   std::fprintf(stderr, "carview: note: no parts database: %s\n", why.c_str());
                   return nullptr;
                 }
                 return db;
               }).share();
  }
  // The career page's PNGs and the HUD: the game's fonts.
  const auto db_cat_t0 = std::chrono::steady_clock::now();
  if (!world_dir.empty() || !game_root_given.empty()) load_ui_fonts(game_root_of());
  // The screen's car names (FORMAT-STRINGTABLES), loaded on first use.
  auto car_names = std::make_shared<race::CarNames>();
  auto car_names_loaded = std::make_shared<bool>(false);
  carview::set_car_names([=](const std::string& media) {
    if (!*car_names_loaded) {
      *car_names_loaded = true;
      const cars::PartsDb* db = parts_db.valid() ? parts_db.get().get() : nullptr;
      std::string e;
      car_names->load(game_root_of(), state_root, &e, language, db);
    }
    return car_names->short_name(media);
  });
  carview::set_game_text([=](const std::string& value) {
    if (!*car_names_loaded) carview::car_name("");  // loads the tables
    return car_names->text(value);
  });
  // The career garage (SPEC-GARAGE-UPGRADES): garage.txt beside the save,
  // made by the profile import; the cars the bake has, by Data_Car id.
  garage::Garage career_garage;
  bool have_garage = false;
  std::filesystem::path garage_path;
  if (!save_path.empty()) garage_path = save_path.parent_path() / "garage.txt";
  auto baked_ids = bake_dir.empty() ? std::map<int64_t, std::string>{} : baked_car_ids(bake_dir);
  auto write_career_garage = [&]() {
    std::string e;
    if (have_garage && !garage_path.empty() && !garage::write_garage(garage_path, career_garage, &e))
      std::fprintf(stderr, "carview: %s\n", e.c_str());
  };
  // The profile import (FORMAT-PROFILE), with the cars the bake has.
  bool imported = false;
  if (!import_dir.empty() && have_saved && saved.owned_garage && !import_again) {
    // A second import would replace what was bought and driven since.
    std::fprintf(stderr,
                 "carview: the save already holds an imported career (%s); drop --import-profile, or use "
                 "--import-profile-again to replace it\n",
                 save_path.string().c_str());
    return 2;
  }
  if (!import_dir.empty()) {
    profile::Imported prof;
    std::string e;
    if (!profile::import_profile(import_dir, prof, &e)) {
      std::fprintf(stderr, "carview: --import-profile: %s\n", e.c_str());
      return 2;
    }
    std::error_code ec;
    const auto& baked = baked_ids;
    std::vector<std::string> notes;
    profile::to_drive_save(prof, [&](int64_t id) { auto it = baked.find(id); return it == baked.end() ? std::string() : it->second; },
                           saved, &notes);
    have_saved = imported = true;
    for (const auto& n : notes) std::printf("import: %s\n", n.c_str());
    std::printf("import: %s: %lld credits, %d wristband points, %zu signs, %zu garage cars, car %s\n",
                import_dir.string().c_str(), (long long)saved.credits, saved.wristband_points, saved.signs_found.size(),
                saved.garage.size(), saved.car.empty() ? "(none baked)" : saved.car.c_str());
    if (!car_given && !saved.car.empty()) car = saved.car;
    if (!save_path.empty() && std::filesystem::exists(save_path, ec)) {
      auto keep = save_path;
      keep += ".pre-import";
      if (!std::filesystem::exists(keep, ec)) std::filesystem::copy_file(save_path, keep, ec);
    }
    career_garage = profile::to_garage(prof);
    have_garage = true;
    if (!garage_path.empty() && std::filesystem::exists(garage_path, ec)) {
      auto keep = garage_path;
      keep += ".pre-import";
      if (!std::filesystem::exists(keep, ec)) std::filesystem::copy_file(garage_path, keep, ec);
    }
    write_career_garage();
  }
  if (!have_garage && have_saved && saved.owned_garage && !garage_path.empty()) {
    std::string e;
    have_garage = garage::read_garage(garage_path, career_garage, &e);
    if (!have_garage && !e.empty()) std::fprintf(stderr, "carview: note: garage ignored: %s\n", e.c_str());
  }
  // The selected garage car is the car (unless --car names one).
  if (have_garage && !car_given)
    if (const garage::Car* gc = career_garage.selected_car())
      if (auto it = baked_ids.find(gc->car_id); it != baked_ids.end()) car = it->second;
  // Class bands from the disc, and shared merged DB handle for career/race queries.
  std::vector<garage::ClassBand> class_bands;
  const cars::PartsDb* shared_db = nullptr;
  if (!world_dir.empty()) {
    const auto game_root = game_root_of();
    std::string e;
    if (!garage::load_class_bands(game_root, class_bands, &e)) std::fprintf(stderr, "carview: note: %s\n", e.c_str());
    if (parts_db.valid()) shared_db = parts_db.get().get();
  }
  if (!mods_off) {
    const size_t mod_cars = carview::extend_baked_catalog(baked_ids, shared_db, mods_session);
    if (mod_cars > 0) {
      std::printf("mod: extended car catalog with %zu mod car(s)\n", mod_cars);
      if (have_garage && !car_given)
        if (const garage::Car* gc = career_garage.selected_car())
          if (auto it = baked_ids.find(gc->car_id); it != baked_ids.end()) car = it->second;
    }
  }
  // Career progression catalog (events, wristbands, restrictions, world triggers) & Skills/Popularity tables.
  race::CareerCatalog career_catalog;
  bool have_career_catalog = false;
  freeroam::SkillsData skills_data;
  bool have_skills_data = false;
  if (!world_dir.empty() && colorado_world()) {
    std::string e;
    have_career_catalog = race::load_career_catalog(game_root_of(), state_root, shared_db, career_catalog, &e);
    if (!have_career_catalog && !e.empty()) std::fprintf(stderr, "carview: note: no career catalog: %s\n", e.c_str());
    e.clear();
    have_skills_data = freeroam::load_skills_data(game_root_of(), skills_data, &e);
    if (!have_skills_data && !e.empty()) std::fprintf(stderr, "carview: note: no skills data: %s\n", e.c_str());
  }
  bench_startup.db_catalog_load_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - db_cat_t0).count();
  // Title screen shown on windowed launch unless bypassed (--no-title, --start-in-single-player, --event, --import-profile).
  bool show_title = !headless && !no_title && !start_in_single_player && race_event.empty() && import_dir.empty();
  carview::FmvPlayer fmv_player;
  carview::StoryIntro story_intro;
  bool boot_intro_checked = false;
  (void)skip_vo_with_a;
  bool title_menu_phase = false;
  bool title_confirm_new = false;
  int title_selected = 0, title_confirm_selected = 1;
  bool new_career_save_pending = false;
  std::string pre_loaner_car;
  int64_t pre_loaner_garage_id = 0;
  // The event's race: its data, and the player's car on its slot (the
  // save does not override the grid).
  std::unique_ptr<carview::RaceView> race;
  race::CareerRecord career;
  race::WristbandTables wristbands;
  double race_top_speed = 0;
  // Opens event `id`'s race and puts the player on its slot; false with
  // `why` (the drive goes on in free roam).
  auto start_race = [&](const std::string& id, std::string& why) {
    race_options.autopilot = race_auto < 0 ? headless : race_auto == 1;
    race_options.player_car = car;  // its stock PI for the AI upgrades (interim: not the garage row)
    race_options.stage_world = world_dir;  // a Rally stage's route and line (--event stage:<route>)
    race_options.state_root = state_root;
    race_options.parts_db = shared_db;
    race_options.language = language;
    auto r = std::make_unique<carview::RaceView>(bake_dir);
    if (!r->open(game_root_of(), id, race_options, why)) return false;
    race = std::move(r);
    race->player_spawn(spawn);
    have_spawn = true;
    race_top_speed = 0;
    if (wristbands.levels.empty()) {
      std::string e;
      if (!race::load_wristband_tables(game_root_of(), state_root, shared_db, wristbands, &e))
        std::fprintf(stderr, "carview: note: no wristband tables: %s\n", e.c_str());
    }
    return true;
  };
  if (!race_event.empty()) {
    if (world_dir.empty()) {
      std::fprintf(stderr, "carview: --event needs the world (--world)\n");
      return 2;
    }
    std::string why;
    if (!start_race(race_event, why)) {
      std::fprintf(stderr, "carview: --event %s: %s\n", race_event.c_str(), why.c_str());
      return 2;
    }
  }
  // The career (SPEC-EVENT-REWARDS) from the save.
  if (have_saved) {
    career.credits = saved.credits, career.total_winnings = saved.total_winnings;
    career.wristband_points = saved.wristband_points;
    career.num_races = saved.num_races, career.num_victories = saved.num_victories, career.num_podiums = saved.num_podiums;
    career.time_in_festival = saved.time_festival, career.time_in_street = saved.time_street;
    career.time_in_exhibition = saved.time_exhibition, career.top_speed = saved.top_speed;
    for (const auto& [id, place] : saved.race_results) career.race_places[id] = place;
    for (const auto& [id, place] : saved.event_results) career.event_places[id] = place;
    career.prize_cars = saved.prize_cars;
    if (saved.difficulty >= 0 && saved.difficulty <= 4) race_options.difficulty = race::Difficulty(saved.difficulty);
    if (saved.ai_mode >= 0 && saved.ai_mode <= 2) race_options.ai_mode = race::AiMode(saved.ai_mode);
  }
  int dynamic_ai_skill = have_saved ? saved.ai_skill : 10;
  if (wristbands.levels.empty() && !world_dir.empty()) {
    std::string e;
    race::load_wristband_tables(game_root_of(), state_root, shared_db, wristbands, &e);
  }
  race::ProgressionTables prog_tables;
  bool have_prog_tables = false;
  if (!world_dir.empty()) {
    std::string pe;
    have_prog_tables = race::load_progression_tables(game_root_of(), state_root, shared_db, prog_tables, &pe);
    if (have_prog_tables)
      std::printf("progression: %zu driver levels, %zu affinity levels loaded\n", prog_tables.driver_levels.size(),
                  prog_tables.affinity_levels.size());
  }
  if (have_saved) {
    career.driver_xp = saved.driver_xp;
    career.driver_level = have_prog_tables ? prog_tables.driver_level_for_xp(career.driver_xp)
                                           : std::max(1, saved.level);
    for (const auto& [mid, xp] : saved.make_affinity) career.make_affinity_xp[mid] = xp;
  }
  int h2h_races = have_saved ? saved.h2h_races : 0;
  int h2h_wins = have_saved ? saved.h2h_wins : 0;
  freeroam::SpeedActivitiesData speed_data;
  std::unique_ptr<freeroam::SpeedActivityTracker> speed_tracker;
  if (!world_dir.empty() && colorado_world()) {
    std::string se;
    if (freeroam::load_speed_activities(game_root_of(), speed_data, &se)) {
      speed_tracker = std::make_unique<freeroam::SpeedActivityTracker>(speed_data);
      if (have_saved) speed_tracker->load_records(saved.speed_traps, saved.speed_zones);
      std::printf("speed activities: %zu traps, %zu zones loaded\n", speed_data.traps.size(), speed_data.zones.size());
    }
  }
  int collected_wristband = 0;
  if (have_saved && saved.collected_wristband >= 0) {
    collected_wristband = saved.collected_wristband;
  } else if (have_career_catalog) {
    collected_wristband = std::max(0, career_catalog.earned_wristband(career.wristband_points));
  } else {
    collected_wristband = std::max(0, wristbands.level_index(career.wristband_points));
  }
  std::unique_ptr<freeroam::SkillTracker> skill_tracker;
  int64_t max_skills_car = have_saved ? saved.max_skills_car : 0;
  std::string sponsor_banner_title, sponsor_banner_detail;
  double sponsor_banner_at = -1e9;
  if (have_skills_data) {
    skill_tracker = std::make_unique<freeroam::SkillTracker>(
        skills_data, have_saved ? saved.popularity_points : 0.0, have_saved ? saved.fame_rewards_claimed : 0,
        have_saved ? saved.max_skills_score : 0, max_skills_car);
    if (have_saved) {
      for (const auto& sp : saved.sponsors) skill_tracker->set_sponsor_progress(sp.id, sp.count, sp.claimed);
    }
  }
  auto current_pop_rank = [&]() -> int { return skill_tracker ? skill_tracker->popularity_rank() : 250; };
  auto class_name_list = [&]() {
    std::vector<std::string> out;
    for (const auto& b : class_bands) out.push_back(b.name);
    return out;
  };
  // Adds a prize/reward/starter car to the career garage and persists garage.txt.
  auto award_garage_car = [&](int64_t car_id, int colour_seq, bool select_it) -> std::string {
    const auto* spec = have_career_catalog ? career_catalog.find_car(car_id) : nullptr;
    std::string media = spec ? spec->media_name : std::string();
    if (media.empty()) {
      auto it = baked_ids.find(car_id);
      if (it != baked_ids.end()) media = it->second;
    }
    if (car_id <= 0 || media.empty()) return {};
    // Ensure garage is enabled.
    have_garage = true;
    saved.owned_garage = true;
    // Check if we already own this exact prize car (when not forced starter).
    if (!select_it) {
      for (const auto& gc : career_garage.cars)
        if (gc.car_id == car_id) return carview::car_name(media);
    }
    std::map<std::string, int64_t> stock;
    if (parts_db.valid()) {
      if (const auto db = parts_db.get()) {
        std::string ignored;
        db->stock_slots(car_id, stock, &ignored);
      }
    }
    garage::Car gc;
    gc.id = ++career_garage.last_id;
    gc.car_id = car_id;
    gc.performance_index = spec ? spec->stock_pi : 0.5;
    gc.class_id = spec ? spec->stock_class : (class_bands.empty() ? 2 : garage::class_of(gc.performance_index, class_bands));
    gc.parts_value = 0;
    gc.colour = colour_seq;
    gc.slots = std::move(stock);
    career_garage.cars.push_back(std::move(gc));
    if (select_it || career_garage.selected <= 0) career_garage.selected = career_garage.cars.back().id;
    write_career_garage();
    return carview::car_name(media);
  };
  // The career page's records (career_page.h): the pause menu's page and
  // --career-png.
  auto career_summary = [&](std::vector<cars::PackInfo>& packs) {
    carview::CareerSummary cs;
    if (wristbands.levels.empty() && !world_dir.empty()) {
      std::string e;
      race::load_wristband_tables(game_root_of(), state_root, shared_db, wristbands, &e);
    }
    const int li = wristbands.level_index(career.wristband_points);
    if (li >= 0) cs.level_name = carview::game_text(wristbands.levels[size_t(li)].second);
    // A name that is a string-table key (the tables are not read: interim):
    // the level's number instead.
    if (cs.level_name.rfind("_&", 0) == 0) cs.level_name = std::to_string(li);
    cs.level = li;
    cs.signs_found = int(saved.signs_found.size());
    cs.barns = int(saved.barns.size());
    for (const auto& b : saved.barns) cs.barns_found += b.status == freeroam::kBarnFound;
    cs.wristband_points = career.wristband_points, cs.credits = career.credits;
    cs.total_winnings = career.total_winnings, cs.races = career.num_races;
    cs.victories = career.num_victories, cs.podiums = career.num_podiums;
    cs.garage_cars = int(career_garage.cars.size());
    cs.club_cars = int(saved.club.size());
    for (const auto& c : saved.club)
      for (const auto& [key, done] : c.challenges) cs.club_total++, cs.club_done += done;
    cs.has_rally = saved.has_rally, cs.rallies = saved.num_rallies, cs.rally_stages = saved.num_rally_stages;
    cs.rally_victories = saved.num_rally_victories, cs.rally_winnings = saved.rally_winnings;
    cs.rally_xp = saved.rally_xp, cs.rally_level = saved.rally_level;
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    packs = cars::list_packs(state ? state : "");
    // SPEC-V4-CONTENT, Progression: Rally needs its package (offer 1600).
    cs.rally_owned = std::any_of(packs.begin(), packs.end(),
                                 [](const cars::PackInfo& p) { return p.puboffer == 1600 && p.owned(); });
    return cs;
  };
  // The career page's board (game_hud.h): the summary with the wristband
  // table's thresholds.
  auto career_board = [&](const carview::CareerPage& page) {
    const carview::CareerSummary& cs = page.summary();
    carview::CareerBoard b;
    if (!cs.level_name.empty()) b.band_name = cs.level_name + " WRISTBAND";
    b.points = cs.wristband_points;
    const int li = wristbands.levels.empty() ? -1 : wristbands.level_index(cs.wristband_points);
    b.band_lo = li >= 0 ? wristbands.levels[size_t(li)].first : 0;
    if (size_t(li + 1) < wristbands.levels.size()) {
      b.band_hi = wristbands.levels[size_t(li + 1)].first;
      b.band_next = carview::game_text(wristbands.levels[size_t(li + 1)].second);
      if (b.band_next.rfind("_&", 0) == 0) b.band_next.clear();
    }
    b.credits = cs.credits, b.winnings = cs.total_winnings;
    b.races = cs.races, b.wins = cs.victories, b.podiums = cs.podiums, b.garage = cs.garage_cars;
    b.popularity_rank = current_pop_rank();
    b.max_skills = skill_tracker ? skill_tracker->max_skills_score() : 0;
    b.driver_level = std::max(1, career.driver_level);
    b.driver_xp = career.driver_xp;
    b.speed_traps = speed_tracker ? int(speed_tracker->traps_completed()) : int(saved.speed_traps.size());
    b.speed_traps_total = int(speed_data.traps.size());
    b.speed_zones = speed_tracker ? int(speed_tracker->zones_completed()) : int(saved.speed_zones.size());
    b.speed_zones_total = int(speed_data.zones.size());
    b.h2h_wins = h2h_wins;
    b.h2h_races = h2h_races;
    b.signs = cs.signs_found, b.barns = cs.barns_found, b.barns_total = cs.barns;
    b.club_cars = cs.club_cars, b.club_done = cs.club_done, b.club_total = cs.club_total;
    b.rally = cs.has_rally, b.rally_level = cs.rally_level, b.rally_xp = cs.rally_xp;
    b.rallies = cs.rallies, b.rally_wins = cs.rally_victories;
    b.packs_owned = page.packs_owned(), b.packs_total = page.packs_total();
    return b;
  };
  // --career-png: every window of the career page as the game's save left
  // it (prefix_N.png), then exit: the M6 gate's view of the import.
  if (!career_png.empty()) {
    std::vector<cars::PackInfo> packs;
    const carview::CareerSummary cs = career_summary(packs);
    carview::CareerPage page;
    page.open(cs, std::move(packs));
    for (int n = 0; n < 8; ++n) {
      HudState st;
      int sel = 0;
      page.fill(st.menu_title, st.menu, sel);
      st.menu_selected = sel;
      const std::string f = career_png.string() + "_" + std::to_string(n) + ".png";
      const carview::CareerBoard board = career_board(page);
      if (!write_menu_png(st, f, &board)) return 1;
      std::printf("career: %s\n", f.c_str());
      if (n == 0)
        std::printf("career:   %s  %d points (next %d)  %lld CR  winnings %lld CR  races %d wins %d podiums %d  "
                    "garage %d  signs %d  barns %d/%d  club %d cars %d/%d  rally %s level %d %d xp %d/%d\n",
                    board.band_name.empty() ? "no wristband" : board.band_name.c_str(), board.points, board.band_hi,
                    static_cast<long long>(board.credits), static_cast<long long>(board.winnings), board.races,
                    board.wins, board.podiums, board.garage, board.signs, board.barns, board.barns_total,
                    board.club_cars, board.club_done, board.club_total, board.rally ? "yes" : "no", board.rally_level,
                    board.rally_xp, board.rally_wins, board.rallies);
      for (const auto& l : st.menu) std::printf("career:   %s\n", l.c_str());
      const size_t before = st.menu.empty() ? 0 : st.menu.size();
      page.move(7);
      HudState probe;
      int sel2 = 0;
      page.fill(probe.menu_title, probe.menu, sel2);
      if (probe.menu == st.menu || before == 0) break;
    }
    return 0;
  }
  if (have_saved && !imported && !have_spawn && (!car_given || lower_ascii(saved.car) == lower_ascii(car))) {
    spawn[0] = saved.position[0], spawn[1] = saved.position[1], spawn[2] = saved.position[2];
    spawn[3] = saved.heading;
    have_spawn = true;
    std::printf("save: %s, %s at %.1f %.1f %.1f\n", save_path.string().c_str(), saved.car.c_str(), spawn[0],
                spawn[1], spawn[2]);
  }
  if (bake_dir.empty() || car.empty()) {
    std::fprintf(stderr, "carview: no bake and car (no play folder found; see --play)\n");
    drive_usage();
    return 2;
  }
  if (view != "chase" && view != "front" && view != "cockpit") {
    std::fprintf(stderr, "carview --drive: --view is chase, front, or cockpit\n");
    return 2;
  }
  bool front = view == "front";
  if (view == "cockpit") camera_mode = int(drive::CameraMode::kCockpit);
  const bool cruise = run == "cruise" && script_path.empty();
  if (cruise && world_dir.empty()) {
    std::fprintf(stderr, "carview --drive: --run cruise needs --world\n");
    return 2;
  }
  if (headless && out.empty()) out = car + "_drive.png";
  WorldReference world_reference;
  if (!world_reference_path.empty()) {
    if (!headless || world_dir.empty()) {
      std::fprintf(stderr, "carview --drive: --world-reference needs --headless and --world\n");
      return 2;
    }
    std::string e;
    if (!read_world_reference(world_reference_path, world_reference, e)) {
      std::fprintf(stderr, "carview: %s\n", e.c_str());
      return 1;
    }
    // Gather around the camera (game frame: the file frame with z negated).
    near_x = world_reference.camera_position[0];
    near_z = -world_reference.camera_position[2];
    near_given = true;
    width = world_reference.width;
    height = world_reference.height;
  }
  std::string text, error;
  if (!script_path.empty()) {
    std::ifstream in(script_path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  } else {
    for (const auto& r : kDriveRuns)
      if (run == r[0]) text = r[1];
    if (text.empty()) {
      std::fprintf(stderr, "carview --drive: no run named %s\n", run.c_str());
      return 2;
    }
  }
  std::vector<drive::ScriptLine> script;
  if (!drive::parse_script(text, script, &error)) {
    std::fprintf(stderr, "carview --drive: %s\n", error.c_str());
    return 1;
  }

  SDL_Window* window = nullptr;
  if (!headless) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
      std::fprintf(stderr, "carview: SDL_Init: %s\n", SDL_GetError());
      return 1;
    }
    // Audio apart: without it the drive goes on silent.
    if (sound && !SDL_InitSubSystem(SDL_INIT_AUDIO))
      std::fprintf(stderr, "carview: no audio (%s); driving without sound\n", SDL_GetError());
    window = SDL_CreateWindow(("carview drive - " + car).c_str(), int(width), int(height),
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
      std::fprintf(stderr, "carview: SDL_CreateWindow: %s\n", SDL_GetError());
      SDL_Quit();
      return 1;
    }
  }
  int status = 0;
  // The car picker (ours): the baked cars by key; picking one restarts the
  // drive with it where the last car stood (window and SDL kept). With a
  // save from the player's profile, only the owned cars the bake has (the
  // garage, barn finds included).
  std::vector<std::string> baked_cars;
  std::vector<int64_t> pick_ids;  // the garage Id of each, with a garage
  std::set<std::string> all_baked;  // every baked car (the Autoshow's)
  std::function<void()> sync_picker;
  // The garage's cars the bake has, by class then index (SPEC-GARAGE-
  // UPGRADES, The active car and switching); again after a purchase.
  auto garage_picker = [&]() {
    std::vector<const garage::Car*> rows;
    for (const auto& c : career_garage.cars)
      if (baked_ids.count(c.car_id)) rows.push_back(&c);
    std::stable_sort(rows.begin(), rows.end(), [](const garage::Car* a, const garage::Car* b) {
      return a->class_id != b->class_id ? a->class_id < b->class_id : a->performance_index < b->performance_index;
    });
    std::vector<std::string> keep;
    std::vector<int64_t> ids;
    for (const auto* c : rows) keep.push_back(lower_ascii(baked_ids.at(c->car_id))), ids.push_back(c->id);
    if (!keep.empty()) baked_cars = std::move(keep), pick_ids = std::move(ids);
    if (sync_picker) sync_picker();
    return rows.size();
  };
  {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(bake_dir / "cars", ec))
      if (e.path().extension() == ".shiftbake") baked_cars.push_back(e.path().stem().string());
    std::sort(baked_cars.begin(), baked_cars.end());
    for (const auto& c : baked_cars) all_baked.insert(lower_ascii(c));
    if (have_garage) {
      const size_t n = garage_picker();
      std::printf("garage: %zu cars, %zu baked\n", career_garage.cars.size(), n);
    } else if (have_saved && saved.owned_garage) {
      const auto& ids = baked_ids;
      std::vector<std::string> owned;
      for (int id : saved.garage)
        if (auto it = ids.find(id); it != ids.end()) owned.push_back(lower_ascii(it->second));
      std::sort(owned.begin(), owned.end());
      owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
      std::vector<std::string> keep;
      for (const auto& c : baked_cars)
        if (std::binary_search(owned.begin(), owned.end(), lower_ascii(c))) keep.push_back(c);
      std::printf("garage: %zu owned cars, %zu baked\n", saved.garage.size(), keep.size());
      if (!keep.empty()) baked_cars = std::move(keep);
    }
  }
  std::string next_car;
  // The pause menu's event pick (ours): every race carview runs, by
  // HorizonEventID; picking one restarts the drive on its grid. After a
  // finish, Continue leaves the race at the route's post-race spot and
  // Restart runs it again (SPEC-EVENT-REWARDS, Results screen).
  std::vector<std::string> event_ids;
  std::map<std::string, std::string> event_names;  // id -> Events.Name reference
  if (!world_dir.empty() && colorado_world()) {
    std::vector<race::EventSummary> all;
    std::string e;
    if (race::list_events(game_root_of(), all, &e, state_root, shared_db))
      for (const auto& ev : all)
        if (ev.ribbon >= 0 && ev.career_type != 9 && ev.environment_id != 111)
          event_ids.push_back(ev.horizon_id), event_names[ev.horizon_id] = ev.name;
  } else if (!world_dir.empty()) {
    // Another world (ColoradoDirt): its stages, one per route file with a
    // loose AI line (SPEC-V4-CONTENT; the heats are route 24).
    std::string w = world_dir.filename().string();
    for (char& ch : w) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (int r = 0; r < 300; ++r) {
      char route[40], line[40];
      std::snprintf(route, sizeof route, "TrackRoute%03d.xml", r);
      std::snprintf(line, sizeof line, "route_%03d.owt", r);
      std::error_code ec;
      if (std::filesystem::exists(world_dir / "Ribbon_00" / route, ec) &&
          std::filesystem::exists(world_dir.parent_path().parent_path() / "aiopenworld" / w / "Ribbon_00" / line, ec))
        event_ids.push_back("stage:" + std::to_string(r));
    }
  }
  size_t event_pick = 0;
  for (size_t k = 0; k < event_ids.size(); ++k)
    if (event_ids[k] == race_event) event_pick = k;
  std::string next_event;
  bool leave_race = false;
  // The free-roam progress outlives car changes; the map loads once.
  freeroam::Collectibles collectibles;
  freeroam::Progress progress;
  bool progress_loaded = false;
  MapView map_view;
  bool map_tried = false;
  traffic::GpsSolver gps_solver;
  traffic::GpsNavigator gps_nav(&gps_solver);
  freeroam::RoadDiscovery road_discovery;
  freeroam::OutpostCatalog outposts;
  freeroam::OutpostProgress outpost_progress;
  freeroam::PrStuntSession pr_stunt_session;
  bool have_outposts = false;
  bool outposts_tried = false;
  bool cli_gps_applied = false;
  bool cli_pr_stunt_applied = false;
  // The audio host (dedicated render thread, mixer, radio, Your Music)
  // outlives car changes so the radio stations keep playing.
  carview::AudioHost audio_host;
  bool radio_logos_tried = false;
  if (have_saved) {
    if (audio_cfg.radio_dj && !saved.radio_dj) audio_cfg.radio_dj = false;
    if (std::fabs(audio_cfg.radio_volume - 1.0f) < 1e-4f && std::fabs(saved.radio_volume - 1.0) > 1e-4)
      audio_cfg.radio_volume = float(saved.radio_volume);
    if (audio_cfg.satnav_on && !saved.satnav_enabled) audio_cfg.satnav_on = false;
    if (saved.treasure_map) treasure_map_setting = true;
    outpost_progress.discovered_outposts.insert(saved.discovered_outposts.begin(), saved.discovered_outposts.end());
    outpost_progress.completed_pr_stunts.insert(saved.completed_pr_stunts.begin(), saved.completed_pr_stunts.end());
  }
  const std::filesystem::path user_music_dir =
      !state_root.empty() ? (state_root / "music")
                          : (!settings_path().empty() ? settings_path().parent_path() / "music" : std::filesystem::path{});
  audio_host.apply_config(audio_cfg, user_music_dir);
  struct Logo {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
  };
  std::map<std::string, Logo> radio_logos;
  // The paint page's last choice (SPEC-AUTOSHOW-PAINT: a Combo_Colors
  // Sequence), kept across the rebuild it asks for.
  int64_t paint_sequence = -1;
  std::string paint_car;
  // The 1000 Club (SPEC-V4-CONTENT): the package's challenges and v4's
  // named locations, loaded the first time a drive asks.
  // 24-hour Time-of-Day, real-time sky (realtimesky.zip + Stars.bin), and
  // PostProcessingZones / 3D LUTs (SPEC-SKY-AND-TOD), loaded once for the world.
  world::TodSystem tod_system;
  world::TodClock tod_clock;
  if (have_saved && !hour_explicit && saved.tod_sec >= 0.0 && saved.tod_sec < 86400.0) {
    tod_clock.tod_sec = saved.tod_sec;
  } else {
    tod_clock.set_hour(hour);
  }
  tod_clock.speed_scale = day_length_scale;
  if (!world_dir.empty()) {
    std::string tod_err;
    if (tod_system.load(world_dir, &tod_err)) {
      std::printf("tod: TimeOfDayA (%zu curves), zones %zu, LUTs %zu, sky %s (%zu stars)\n",
                  tod_system.preset_a.curves.size(), tod_system.zones.zones.size(), tod_system.zones.luts.size(),
                  tod_system.loaded_sky ? "realtimesky.zip" : "procedural", tod_system.sky_assets.stars.size());
    }
  }
  // Particle effects (media/effects.zip, Ribbon_00/ParticleEmitters.xml,
  // SmashableEffects.xml, audio/BackfireTimerConfig.xml), loaded once.
  world::ParticleLibrary vfx_lib;
  world::SmashableWorld smashables;
  world::AnimatedWorldSystem world_anim;
  {
    const auto groot = game_root_of();
    if (!groot.empty()) {
      std::string vfx_err;
      if (vfx_lib.load(groot / "media", world_dir, &vfx_err)) {
        std::printf("vfx: %zu effects, %zu xds textures, %zu world emitters, %zu smashable effects\n",
                    vfx_lib.effects().size(), vfx_lib.xds_count(), vfx_lib.world_emitters().size(),
                    vfx_lib.smashable_effects().size());
      }
      std::string sm_err;
      if (smashables.load(groot / "media", world_dir, &sm_err)) {
        const auto c = smashables.definitions().counts();
        std::printf("smashables: %zu defs (%zu children), %zu CollObjects types, %zu placed props\n",
                    c.top_level, c.total_children, smashables.coll_object_types().size(),
                    smashables.placed_prop_count());
      }
      std::string anim_err;
      if (world_anim.init(groot / "media", tod_system.post_config, &anim_err)) {
        const auto& cat = world_anim.catalog();
        std::printf("world_anim: %zu spectator models (%d angles), %zu crowdclasses, %zu animated pgeo / %zu xds\n",
                    cat.models.size(), cat.num_sprite_angles, cat.crowdclasses.size(), cat.animated_pgeos,
                    cat.animated_xds);
      }
    }
  }
  freeroam::club::ClubData club_data;
  bool club_tried = false, club_loaded = false;
  std::string club_why;
  // Horizon Rally Expansion (Slice 8): seamless Colorado <-> ColoradoDirt world
  // transition, 7x4 Championships + RALLY_HEATS, Service Area & Co-Driver VO.
  const std::filesystem::path colorado_world_dir = world_dir;
  bool has_colorado_return = false;
  std::array<double, 3> colorado_return_pos{};
  double colorado_return_heading = 0.0;
  race::RallyCatalog rally_catalog;
  bool rally_catalog_tried = false;
  race::RallyChampionshipSession rally_session;
  bool rally_hub_open = false;
  int rally_hub_selected = 0;
  bool rally_service_open = false;
  int rally_service_selected = 0;
  bool rally_service_repaired = false;
  bool rally_results_open = false;
  carview::RallyChampResultsState rally_results_state{};
  int rally_world_switch = 0;  // 0 none, +1 enter ColoradoDirt, -1 return to Colorado
  std::string rally_notice;
  for (bool again = true; again;) {
  again = false;
  if (skill_tracker) skill_tracker->reset_session_clock();
  {
    Renderer renderer;
    Showroom showroom(renderer);
    // SHIFT_NO_CULL=1: every world draw, for comparison with the culled frame.
    if (const char* e = std::getenv("SHIFT_NO_CULL"); e && *e == '1') showroom.set_world_culling(false);
    ShowroomOptions options;
    options.game_root = game_root_of();
    // The garage car's paint & fitted visual upgrades (body kits, wing, rims, brakes, stance).
    if (have_garage)
      if (const garage::Car* gc = career_garage.selected_car())
        if (auto it = baked_ids.find(gc->car_id); it != baked_ids.end() && lower_ascii(it->second) == lower_ascii(car)) {
          options.match_sequence = gc->colour;
          options.car_id = gc->car_id;
          if (parts_db.valid()) {
            if (const auto db = parts_db.get()) {
              const float rh_f = float(gc->tune.front_ride_height);
              const float rh_r = float(gc->tune.rear_ride_height);
              const float cam_f = float(gc->tune.front_camber_deg);
              const float cam_r = float(gc->tune.rear_camber_deg);
              cars::resolve_visual_setup(*db, gc->car_id, garage::fitted(*gc), options.visual, nullptr, rh_f, rh_r,
                                         cam_f, cam_r);
            }
          }
        }
    // A colour picked in the pause menu's paint page for this car.
    if (paint_sequence >= 0 && lower_ascii(paint_car) == lower_ascii(car)) options.match_sequence = paint_sequence;
    Renderer::Options render_options{window, validation, msaa};
    render_options.reverse_z = reverse_z;
    render_options.backface_cull = backface_cull;
    render_options.z_prepass = z_prepass;
    cli_graphics = carview::modern::clamp_graphics(cli_graphics);
    render_options.graphics = cli_graphics;
    render_options.vsync = cli_graphics.vsync;
    showroom.set_texture_cap(cli_graphics.texture_cap);
    if (cli_graphics.auto_hdr && !headless) {
      const auto st_dir = carview::settings_path().parent_path();
      if (!st_dir.empty()) carview::set_display_hdr_restore_file(st_dir / "hdr_restore.bin");
      carview::turn_on_display_hdr();
    }
    // Anisotropic filtering of the ground's grazing textures (ours): the low preset stays cheap.
    render_options.anisotropy = low_preset ? 2.0f : 16.0f;
    if (post_on) {
      PostSettings& post = render_options.post;
      post.enabled = true;
      if (!world_dir.empty()) read_post_settings(world_dir, post);
      post.exposure = float(exposure_given > 0 ? exposure_given : kPostExposure);
      post.bloom = bloom_flag >= 0 ? bloom_flag == 1 : !low_preset;
      post.balance[0] = kPostBalance[0], post.balance[1] = kPostBalance[1], post.balance[2] = kPostBalance[2];
      if (const char* b = std::getenv("SHIFT_BALANCE"))  // tuning: r,g,b
        std::sscanf(b, "%f,%f,%f", &post.balance[0], &post.balance[1], &post.balance[2]);
    }
    {
      const std::string level = !shadows_flag.empty() ? shadows_flag : low_preset ? "low" : "high";
      ShadowSettings& shadow = render_options.shadow;
      if (level == "low") shadow.size = 1024, shadow.radius = 40, shadow.taps = 1;
      else if (level == "high") shadow.size = 2048, shadow.radius = 60, shadow.taps = 4;
    }
    // The shadow pass casts from the track's shadow light when it overrides
    // the sun (towards the light: the travel direction negated).
    float shadow_light_game[3] = {0, 1, 0};
    bool have_shadow_light = false;
    if (!world_dir.empty() && read_shadow_direction(world_dir, shadow_light_game)) {
      have_shadow_light = true;
      for (float& c : shadow_light_game) c = -c;
    }
    if (const auto groot = game_root_of(); !groot.empty()) {
      const auto xpr = groot / "media" / "tracks" / "uiautoshow" / "staticcarcubemap.xpr";
      if (std::filesystem::exists(xpr)) showroom.set_environment_xpr(xpr);
    }
    const auto car_load_t0 = std::chrono::steady_clock::now();
    const bool car_loaded_ok = showroom.load(bake_dir, car, options, error);
    const auto vk_init_t0 = std::chrono::steady_clock::now();
    const bool vk_init_ok = car_loaded_ok && renderer.init(render_options, error);
    const auto car_up_t0 = std::chrono::steady_clock::now();
    const bool car_up_ok = vk_init_ok && showroom.upload(error);
    const auto car_up_t1 = std::chrono::steady_clock::now();
    if (car_up_ok) {
      const double load_ms = std::chrono::duration<double, std::milli>(vk_init_t0 - car_load_t0).count();
      const double vk_ms = std::chrono::duration<double, std::milli>(car_up_t0 - vk_init_t0).count();
      const double up_ms = std::chrono::duration<double, std::milli>(car_up_t1 - car_up_t0).count();
      bench_startup.vulkan_init_ms = std::max(0.0, vk_ms - renderer.pipeline_compile_ms());
      bench_startup.car_bake_load_ms = load_ms + std::max(0.0, up_ms - showroom.pipeline_compile_ms());
    }
    if (!car_up_ok) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      status = 1;
    } else {
      renderer.set_hdr_pattern(cli_hdr_pattern, cli_hdr_pattern_nits);
      bool user_lut_active = false;
      bool perf_overlay_open = cli_perf_overlay;
      bool inspector_open = cli_frame_inspector;
      carview::InspectorView inspector_view = cli_inspector_view;
      uint32_t temporal_frame_idx = 0;
      double last_frame_cpu_ms = -1.0;
      std::array<float, 4> inspector_preview_uv{};
      bool has_inspector_preview = false;
      if (perf_overlay_open || inspector_open) renderer.set_gpu_timing(true);
      renderer.set_inspector_preview(inspector_open, inspector_view);
      carview::modern::GraphicsSettings active_graphics = cli_graphics;
      carview::modern::FramePacer frame_pacer;
      int display_hz = 60;
      if (window) {
        if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
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
      if (tod_system.loaded_tod) {
        std::string sky_err;
        if (!showroom.upload_sky(tod_system.sky_assets, sky_err))
          std::fprintf(stderr, "carview: sky upload note: %s\n", sky_err.c_str());
      }
      {
        const auto groot = game_root_of();
        if (!groot.empty()) {
          showroom.load_car_lamps(groot, car);
          std::string cl_err;
          if (!showroom.upload_carlights(groot / "media", cl_err))
            std::fprintf(stderr, "carview: carlights upload note: %s\n", cl_err.c_str());
        }
        std::string vfx_gpu_err;
        if (!showroom.init_vfx(&vfx_lib, vfx_gpu_err))
          std::fprintf(stderr, "carview: vfx gpu init note: %s\n", vfx_gpu_err.c_str());
      }
      world::ParticleSystem particle_sys;
      particle_sys.set_library(&vfx_lib);
      world::SkidMarkSystem skid_sys;
      uint32_t last_audio_backfire_count = audio_host.telemetry().backfire_count;
      int last_vfx_gear = 1;
      double last_impact_vfx_time = -10.0;
      // SHIFT_PAINT_PREVIEW=i (debug): the paint page's live preview of
      // colour row i, as left and right show it.
      if (const char* pp = std::getenv("SHIFT_PAINT_PREVIEW")) showroom.preview_colour(size_t(std::atoi(pp)));
      const auto& info = showroom.car();
      // Apply any custom Paint Shop / Vinyl Livery (.liv) saved on the active garage car.
      if (have_garage) {
        if (const garage::Car* gc = career_garage.selected_car()) {
          if (!gc->livery_file.empty() && baked_ids.count(gc->car_id) &&
              lower_ascii(baked_ids.at(gc->car_id)) == lower_ascii(car)) {
            std::filesystem::path liv_p(gc->livery_file);
            if (liv_p.is_relative() && !save_path.empty()) liv_p = save_path.parent_path() / liv_p;
            cars::LiveryDocument ldoc;
            if (cars::load_livery_file(liv_p, ldoc, nullptr)) {
              showroom.set_paint_spec(ldoc.paint);
              if (!ldoc.layers.empty()) {
                cars::LiveryLibrary llib;
                const auto gr = game_root_of();
                if (!gr.empty()) {
                  const auto lzip = gr / "media" / "Livery.zip";
                  if (std::filesystem::exists(lzip)) llib.open(lzip, nullptr);
                }
                std::vector<uint8_t> liv_rgba;
                if (cars::composite_livery(ldoc, llib.is_open() ? &llib : nullptr,
                                           showroom.livery_planes().empty() ? nullptr : &showroom.livery_planes(),
                                           liv_rgba, 1024)) {
                  std::string lerr;
                  showroom.set_custom_livery(liv_rgba.data(), 1024, 1024, lerr);
                }
              }
            }
          }
        }
      }
      std::vector<std::string> physics_notes;
      audio::UpgradeLevels audio_upgrades{};
      int audio_trans_level = 0, audio_turbo_level = 0, audio_sc_level = 0;
      drive::Vehicle vehicle([&] {
        // The selected garage car with its fitted parts (SPEC-GARAGE-UPGRADES,
        // What a part changes).
        if (have_garage && parts_db.valid())
          if (const garage::Car* gc = career_garage.selected_car())
            if (auto it = baked_ids.find(gc->car_id);
                it != baked_ids.end() && lower_ascii(it->second) == lower_ascii(car)) {
              const auto t0 = std::chrono::steady_clock::now();
              const auto db = parts_db.get();
              cars::CarPhysics rows;
              std::string e = "no parts database";
              if (db && db->physics(gc->car_id, garage::fitted(*gc), rows, &e)) {
                audio_upgrades.intake = int(rows.get("List_UpgradeEngineIntake.Level", 0));
                audio_upgrades.exhaust = int(rows.get("List_UpgradeEngineExhaust.Level", 0));
                audio_trans_level = int(rows.get("List_UpgradeDrivetrainTransmission.Level", 0));
                audio_turbo_level = int(rows.get("List_UpgradeEngineTurboSingle.Level", 0));
                audio_sc_level = int(rows.get("List_UpgradeEngineCSC.Level", 0));
                std::printf("garage: car %lld (%s) with its parts, %.0f kg (%.1f s)\n", (long long)gc->id,
                            it->second.c_str(), rows.get("List_UpgradeCarBodyWeight.Mass", 0),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return drive::params_for_fitted_car(info, bake_dir, rows, &physics_notes, &gc->tune);
              }
              physics_notes.push_back("garage parts not applied: " + e);
            }
        return drive::params_for_baked_car(info, bake_dir, &physics_notes);
      }());
      for (const auto& n : physics_notes) std::fprintf(stderr, "carview: physics: %s\n", n.c_str());
      // Engine audio (dedicated render thread, runtime XMA2 decode fallback, L0-L3 upgrades).
      constexpr int kAudioRate = carview::AudioHost::kAudioRate;
      if (sound && (!headless || !audio_out.empty() || bench_audio || bench_full_fx)) {
        const auto aud_t0 = std::chrono::steady_clock::now();
        std::string sound_note;
        if (audio_host.load_car(sound_dir, game_root_of(), state_root, car, audio_upgrades, vehicle.params().idle_rpm,
                                vehicle.params().redline_rpm, &sound_note))
          std::printf("%s\n", sound_note.c_str());
        else
          std::fprintf(stderr, "carview: no engine sound (%s)\n", sound_note.c_str());
        bench_startup.audio_load_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aud_t0).count();
      } else {
        audio_host.clear_car();
      }
      // The sound's inputs from the engine: its throttle, and its torque as
      // a fraction of full throttle (driving) or of closed throttle (braking).
      auto sound_inputs = [&](audio::View audio_view) {
        audio::EnginePlayer::Update u;
        const double rpm = vehicle.rpm(), thr = vehicle.engine_throttle();
        const double torque = vehicle.engine_torque(rpm, thr);
        const double full = vehicle.engine_torque(rpm, 1), closed = vehicle.engine_torque(rpm, 0);
        u.rpm = rpm;
        u.throttle = thr;
        u.pos_torque = torque > 0 && full > 0 ? torque / full : 0;
        u.neg_torque = torque < 0 && closed < 0 ? torque / closed : 0;
        u.view = audio_view;
        return u;
      };
      std::vector<float> audio_track;  // headless --audio-out
      const drive::FlatGround flat;
      const drive::Ground* ground = &flat;
      world::World colorado;
      world::Collision collision;
      std::unique_ptr<world::Streamer> streamer;
      std::unique_ptr<freeroam::FreeRoam> free_roam;
      std::shared_ptr<const std::set<std::string>> hidden_sent;
      WorldTextureLoader texture_loader;
      world::RoadFollower follower(cruise_kmh / 3.6);
      drive::Vec3 start{};
      double start_heading = 0;
      const bool streamer_async = !headless || async_stream;
      // What the streaming worker prepares with each gather: the staged
      // scenery and the road follower's points.
      struct Prepared {
        std::shared_ptr<Showroom::StagedWorld> staged;
        world::RoadFollower::Points road;
        std::vector<world::WorldGlow> glows;
      };
      // The streamer's prepared gather, swapped in.
      auto set_staged_world = [&]() {
        auto prepared = std::any_cast<std::shared_ptr<Prepared>>(std::move(streamer->prepared()));
        streamer->prepared().reset();
        if (!prepared) {
          error = "world staging buffers";
          return false;
        }
        follower.set_points(std::move(prepared->road));
        showroom.set_world_glows(std::move(prepared->glows));
        auto staged = std::move(prepared->staged);
        if (!staged) {
          error = "world staging buffers";
          return false;
        }
        // The first gather and synchronous streams swap at once; later
        // background ones keep the old scenery drawn until the GPU copy is
        // done (poll_world, each step).
        return streamer_async && streamer->gathers() > 1 ? showroom.begin_world(std::move(*staged), error)
                                                         : showroom.set_world(std::move(*staged), error);
      };
      if (!world_dir.empty()) {
        const auto world_load_t0 = std::chrono::steady_clock::now();
        if (cache_path.empty()) cache_path = std::filesystem::path("out") / "world" / (world::model_prefix(world_dir).substr(0, world::model_prefix(world_dir).size() - 3) + ".index");
        world::Spawn sp;
        std::string warning;
        const char* how = "";
        if (!colorado.open(world_dir, cache_path, &error)) {
          status = 1;
        } else if (!render_ground && !collision.open(world_dir, (game_root_given.empty() ? world_dir.parent_path().parent_path() : game_root_of() / "media") / "physics.zip",
                                                     &error, &warning)) {
          status = 1;
        } else {
          if (!error.empty()) std::fprintf(stderr, "carview: note: %s\n", error.c_str());
          if (!warning.empty()) std::fprintf(stderr, "carview: note: %s", warning.c_str());
          error.clear();
          // SHIFT_SURFACES=1: the surface table (the numbers in traces and logs).
          if (std::getenv("SHIFT_SURFACES"))
            for (size_t i = 0; i < collision.surfaces().size(); ++i)
              std::printf("surface %zu %s grip %.2f\n", i, collision.surfaces()[i].name.c_str(),
                          collision.grip(int(i)));
          const int active_route = gather_route_given ? gather_route : (race ? race->event().route_id : 0);
          if (!render_ground) collision.set_active_route(active_route);
          world::GatherOptions gather_options;
          gather_options.render_data = !flat_world;
          gather_options.props = !no_props;
          gather_options.route = active_route;
          if (!flat_world && !no_far) gather_options.far_radius = 3000;
          if (const char* fi = std::getenv("SHIFT_FAR_INNER")) gather_options.far_inner = std::atof(fi);
          if (!flat_world && !no_trees) gather_options.tree_radius = kTreeRadius;
          if (!flat_world) {
            // Popping (ours): the gather reaches further and the props
            // cross-fade by distance from the eye. The eye can be a regather
            // step (plus the swap's lag) from the gather's centre, so the
            // props fade out by kPropRadius - step - lag, where every gather
            // still holds them, and LOD00 is gathered to the LOD band's end
            // plus that much.
            const double prop_radius = low_preset ? kLowPropRadius : kPropRadius;
            gather_options.radius = low_preset ? kLowModelRadius : kModelRadius;
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
            if (!headless || std::getenv("SHIFT_SWAP_FADE")) showroom.set_world_swap_fade(kSwapFadeSeconds);
          }
          // Free roam: found signs and each barn's other door variant are
          // left out of the gathers from the first one on.
          if (collectibles_on && (!bench || bench_full_fx) && !flat_world && colorado_world()) {
            const auto game_root = game_root_of();
            if (collectibles.signs.empty()) {
              std::string why;
              if (!freeroam::load_collectibles(game_root, collectibles, &why)) {
                std::fprintf(stderr, "carview: note: no collectibles: %s\n", why.c_str());
                collectibles = {};
              }
            }
            if (!collectibles.signs.empty()) {
              if (!progress_loaded) {
                if (have_saved) progress_from_save(saved, collectibles, progress);
                progress.total_distance += seed_distance;
                progress_loaded = true;
              }
              // SHIFT_LIST_SIGNS=1: each sign's id and position (for --spawn).
              // Each also says whether car collision lies within 2 m of it
              // (a sign is found by touching it).
              if (std::getenv("SHIFT_LIST_SIGNS")) {
                int reachable = 0;
                for (const auto& sg : collectibles.signs) {
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
                std::printf("signs: %d of %zu with collision within 2 m\n", reachable, collectibles.signs.size());
              }
              free_roam = std::make_unique<freeroam::FreeRoam>(collectibles, progress);
              gather_options.hidden = hidden_sent = free_roam->hidden();
              if (!world_dir.empty() && !collectibles.barns.empty()) {
                std::vector<world::BarnDoorCollider> bspecs;
                bspecs.reserve(collectibles.barns.size());
                for (const auto& b : collectibles.barns) {
                  world::BarnDoorCollider bd;
                  bd.barn_name = b.name;
                  bd.closed_coll_id = b.closed_coll_id;
                  bd.open_coll_id = b.open_coll_id;
                  bspecs.push_back(std::move(bd));
                }
                if (smashables.load_barn_doors(world_dir / "Ribbon_00" / "GameObjs.xml", bspecs) && hidden_sent) {
                  smashables.sync_barns_from_hidden(*hidden_sent);
                }
              }
              std::printf("free roam: %d of %zu signs, %d of %zu barn finds, %.1f km driven\n",
                          free_roam->signs_found(), collectibles.signs.size(), free_roam->barns_found(),
                          collectibles.barns.size(), progress.total_distance / 1000);
            }
            if (!map_tried) {
              map_tried = true;
              std::string why;
              if (!map_view.open(game_root, &why)) std::fprintf(stderr, "carview: note: no map: %s\n", why.c_str());
              traffic::NavMap nav_for_gps;
              if (traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", nav_for_gps, nullptr)) {
                gps_solver.build(nav_for_gps);
                if (road_discovery.build(nav_for_gps) && have_saved && !saved.road_discovery_words.empty()) {
                  road_discovery.load_words(saved.road_discovery_nodes, saved.road_discovery_words);
                }
                if (road_discovery.ready()) {
                  std::printf("roads discovered: %d / %d\n", road_discovery.discovered_count(),
                              road_discovery.total_roads());
                }
              }
            }
            if (!outposts_tried) {
              outposts_tried = true;
              std::string owhy;
              if (freeroam::load_outposts(game_root, outposts, &owhy)) {
                have_outposts = true;
                std::printf("outposts: %zu outposts (%d discovered), %zu PR stunts (%d completed)\n",
                            outposts.outposts.size(), outpost_progress.discovered_count(outposts),
                            outposts.stunts.size(), outpost_progress.completed_stunt_count(outposts));
              } else {
                std::fprintf(stderr, "carview: note: no outposts: %s\n", owhy.c_str());
              }
            }
            if (has_cli_gps && !cli_gps_applied && gps_solver.ready()) {
              cli_gps_applied = true;
              const double sx0 = have_spawn ? spawn[0] : (near_given || render_ground ? near_x : world::kSpawnX);
              const double sz0 = have_spawn ? spawn[2] : (near_given || render_ground ? near_z : world::kSpawnZ);
              gps_nav.set_destination(cli_gps_x, cli_gps_z, traffic::GpsDestKind::kGeneric, "GPS WAYPOINT", sx0, sz0);
            }
            if (!cli_pr_stunt.empty() && !cli_pr_stunt_applied && have_outposts) {
              cli_pr_stunt_applied = true;
              if (const auto* st = outposts.find_stunt(cli_pr_stunt)) {
                const int64_t base_pts = skill_tracker ? int64_t(std::llround(skill_tracker->popularity_points())) : 0;
                pr_stunt_session.start(*st, base_pts);
                if (gps_solver.ready()) {
                  const double sx0 = have_spawn ? spawn[0] : (near_given || render_ground ? near_x : world::kSpawnX);
                  const double sz0 = have_spawn ? spawn[2] : (near_given || render_ground ? near_z : world::kSpawnZ);
                  gps_nav.set_destination(st->target_pos.x, st->target_pos.z, traffic::GpsDestKind::kGeneric,
                                          st->title, sx0, sz0);
                }
              }
            }
          }
          // The radio and Your Music: initialized once, streamed off-thread.
          if (sound && (!bench || bench_audio || bench_full_fx) && (!headless || !audio_out.empty() || bench_audio || bench_full_fx)) {
            const auto aud_world_t0 = std::chrono::steady_clock::now();
            const auto game_root = game_root_of();
            if (radio_on && audio_host.init_radio(game_root, language, have_saved ? saved.radio_station : "", !headless) &&
                !radio_logos_tried) {
              radio_logos_tried = true;
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
                  radio_logos[station] = {std::move(x.levels[0].rgba), x.levels[0].width, x.levels[0].height};
                }
              }
            }
            if (!headless || bench_audio || bench_full_fx) {
              audio_host.init_user_music(user_music_dir, true);
              const auto sfx_cache = !sound_dir.empty() ? sound_dir : (state_root / "sound-cache");
              audio_host.init_sfx(game_root, sfx_cache, audio_host.car_burble_bank());
              audio_host.init_satnav(game_root, sfx_cache, language, true);
              audio_host.init_vo(game_root, language);
              audio_host.init_acoustics(game_root);
            }
            bench_startup.audio_load_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aud_world_t0).count();
          }
          streamer = std::make_unique<world::Streamer>(colorado, gather_options, kRegather,
                                                       render_ground ? nullptr : &collision);
          // Gathers after the first run on a worker in the window (and with
          // --async-stream headless), with the scenery's draw data built
          // there too; headless runs stay synchronous so they repeat.
          streamer->set_async(streamer_async);
          // The worker builds the scenery's draw data and stages it in
          // host-visible buffers; the swap only copies them on the GPU.
          streamer->set_prepare([&showroom, &colorado, &texture_loader](const world::Mesh& mesh, double cx,
                                                                        double cz) {
            auto staged = std::make_shared<Showroom::StagedWorld>();
            std::string e;
            std::vector<uint32_t> pin_records;
            WorldTriangles triangles = scenery(mesh, cx, cz);
            texture_loader.load(colorado, triangles, showroom, pin_records);
            std::shared_ptr<const void> pin_guard(
                reinterpret_cast<const void*>(1),
                [&showroom, pins = std::move(pin_records)](const void*) { showroom.release_prepare_pins(pins); });
            if (!showroom.stage_world(build_world(triangles), *staged, e, cx, cz)) staged.reset();
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
          const double sx = have_spawn ? spawn[0] : near_given || render_ground ? near_x : world::kSpawnX;
          const double sz = have_spawn ? spawn[2] : near_given || render_ground ? near_z : world::kSpawnZ;
          const double near[2] = {near_x, near_z};
          if (!streamer->update(sx, sz, nullptr, &error)) {
            status = 1;
          } else {
            if (fast_travel) {
              fast_travel = false;
              drive::Hit h;
              if (streamer->ground().raycast({spawn[0], 2000.0, spawn[2]}, {0, -1, 0}, 4000.0, h)) {
                spawn[1] = 2000.0 - h.distance;
              } else {
                world::Spawn fsp;
                if (world::find_spawn(streamer->mesh(), spawn[0], spawn[2], fsp)) spawn[1] = fsp.position[1];
              }
            }
            if (!world::choose_spawn(*streamer, have_spawn ? spawn : nullptr, near_given ? near : nullptr,
                                     near_given ? nullptr : &near_heading, sp, &how)) {
              error = "no road triangle near the search point";
              status = 1;
            }
          }
        }
        if (status == 0) {
          std::printf("spawn %s: %.3f %.3f %.3f heading %.4f rad\n", how, sp.position[0], sp.position[1],
                      sp.position[2], sp.heading);
          start = {sp.position[0], sp.position[1], sp.position[2]};
          start_heading = sp.heading;
          vehicle.reset(start, start_heading);
          // "Back to the start" keeps the saved spawn, not where the save left the car.
          if (have_saved && saved.has_spawn) {
            start = {saved.spawn[0], saved.spawn[1], saved.spawn[2]};
            start_heading = saved.spawn_heading;
          }
          ground = &streamer->ground();
          follower.set_ground(&streamer->ground());
          if (!set_staged_world())  // and the follower's road points
            status = 1;
          else
            showroom.set_background(0.42f, 0.55f, 0.72f);
        }
        if (status != 0) std::fprintf(stderr, "carview: %s\n", error.c_str());
        bench_startup.world_load_ms = std::max(
            0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - world_load_t0).count() -
                     bench_startup.audio_load_ms);
      }
      std::unique_ptr<TrafficView> traffic;
      // The race's models go with this scope (before the renderer).
      struct RaceModels {
        carview::RaceView* r;
        ~RaceModels() {
          if (r) r->release_models();
        }
      } race_models{race.get()};
      if (status == 0 && race) {
        std::string e;
        if (!streamer || !race->load_cars(renderer, *ground, e)) {
          std::fprintf(stderr, "carview: race: %s\n", streamer ? e.c_str() : "needs the streamed world");
          status = 1;
        }
        if (status == 0 && !render_ground) race->clip_room(collision);
      }
      if (status == 0 && streamer && colorado_world() &&
          (traffic_flag == 1 || (traffic_flag < 0 && !headless && (!race || race->uses_traffic())))) {
        // The test driver keeps to the right lane among traffic (SPEC-TRAFFIC,
        // Driving a lane: 2.2 m right of the centre line).
        follower.set_keep_right(2.2);
        traffic = std::make_unique<TrafficView>(renderer, showroom, bake_dir, headless && !async_stream);
        const auto game_root = game_root_of();
        if (!traffic->open(game_root, &ground, hour, traffic_seed, error, race != nullptr)) {
          std::fprintf(stderr, "carview: note: no traffic: %s\n", error.c_str());
          error.clear();
          traffic.reset();
        }
      }
      // Free-roam Festival AI Racers & instant Head-to-Head (AIOpenWorld.xml cartype="festival").
      traffic::NavMap festival_nav;
      traffic::Roads festival_roads;
      std::unique_ptr<freeroam::FestivalManager> festival_mgr;
      std::map<std::string, std::unique_ptr<Showroom>> festival_guests;
      struct FestivalGuestsGuard {
        Showroom* host;
        std::map<std::string, std::unique_ptr<Showroom>>* map;
        ~FestivalGuestsGuard() {
          if (host) host->set_instances({});
          if (map) map->clear();
        }
      } festival_guard{&showroom, &festival_guests};
      if (status == 0 && streamer && colorado_world() && !race && (!headless || demo_h2h)) {
        std::string ferr;
        traffic::TrafficConfig fcfg;
        const auto game_root = game_root_of();
        if (traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", festival_nav, &ferr) &&
            traffic::read_traffic_config(game_root, fcfg, &ferr, false)) {
          festival_roads.build(festival_nav);
          std::set<int64_t> traffic_ids;
          for (const auto& tm : fcfg.models) traffic_ids.insert(tm.id);
          std::vector<freeroam::FestivalRacerSpec> roster;
          for (const auto& [cid, media] : baked_ids) {
            if (traffic_ids.count(cid) || media == car) continue;
            freeroam::FestivalRacerSpec sp;
            sp.car_id = cid;
            sp.car_media = media;
            if (have_career_catalog) {
              if (const auto* cs = career_catalog.find_car(cid)) {
                sp.pi = !class_bands.empty() ? garage::shown_index(cs->stock_pi, class_bands) : int(cs->stock_pi);
                const int cl = !class_bands.empty() ? garage::class_of(cs->stock_pi, class_bands) : cs->stock_class;
                if (cl >= 0 && size_t(cl) < class_bands.size()) sp.car_class = class_bands[size_t(cl)].name;
              }
            }
            if (sp.car_class.empty()) sp.car_class = "B";
            roster.push_back(std::move(sp));
            if (roster.size() >= 12) break;
          }
          if (!roster.empty()) {
            const size_t roster_n = roster.size();
            festival_mgr = std::make_unique<freeroam::FestivalManager>(festival_nav, festival_roads, fcfg, traffic_seed + 17);
            festival_mgr->set_roster(std::move(roster));
            festival_mgr->set_hour(hour);
            festival_mgr->set_wristband_level(collected_wristband);
            festival_mgr->set_ground([&](double x, double z, double hint, double& y) {
              if (!ground) return false;
              drive::Hit h;
              if (!ground->raycast({x, hint + 4, z}, {0, -1, 0}, 12, h)) return false;
              y = hint + 4 - h.distance;
              return true;
            });
            std::printf("festival AI: roaming roster ready (%zu models)\n", roster_n);
          }
        }
      }
      // One fixed step, then the stream: gathers again (and swaps the
      // ground and the scenery) once the car is far from the last centre.
      // R and the headless driver's "stuck": a clear spot on the nav road.
      carview::SafeReset safe_reset;
      bool safe_reset_ok = false;
      if (status == 0 && streamer) {
        std::string why;
        safe_reset_ok = safe_reset.open(game_root_of(), &why);
        if (!safe_reset_ok) std::fprintf(stderr, "carview: note: reset on the nearest road triangle (%s)\n", why.c_str());
      }
      // The contacts log: a hard contact is one point pushing with more than
      // half the car's weight; one line when it starts or moves on 3 m.
      struct ContactLog {
        FILE* f = nullptr;
        carview::DrawnIndex drawn, all;  // upright drawn triangles; every drawn triangle
        int gathers = -1;
        bool touching = false;
        drive::Vec3 last{};
        size_t contacts = 0, undrawn = 0;
        ~ContactLog() {
          if (!f) return;
          std::fprintf(f, "summary: %zu hard contacts, %zu with nothing drawn within 1 m\n", contacts, undrawn);
          std::printf("contacts: %zu hard, %zu with nothing drawn within 1 m\n", contacts, undrawn);
          std::fclose(f);
        }
      } contact_log;
      // --texture-census (headless): the scenery's triangles drawn with a
      // fallback (a slot with no texture file, a texture that failed to
      // decode, a terrain layer missing), by model, effect and slot, with
      // their area on screen in pixels (clipped to the view, not occluded).
      auto census_frame = [&](uint32_t width, uint32_t height) {
        const auto& mesh = streamer->mesh();
        if (!mesh.has_render_data()) return;
        const ShowroomFrame& fr = showroom.frame();
        const float* tr = showroom.world_transform();
        float clip[16];
        for (int c = 0; c < 4; ++c)
          for (int r = 0; r < 4; ++r) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += fr.view_projection[k * 4 + r] * tr[c * 4 + k];
            clip[c * 4 + r] = sum;
          }
        const double ox = showroom.world_origin_x(), oz = showroom.world_origin_z();
        auto failed = [&](uint32_t record) {
          return record == world::kNoTexture || texture_loader.failed.count(record) != 0;
        };
        // Why a material falls back, or empty.
        std::vector<std::string> why(mesh.materials.size());
        for (size_t i = 0; i < mesh.materials.size(); ++i) {
          const auto& m = mesh.materials[i];
          auto slot = [&](int k, uint32_t record) {
            char b[96];
            std::snprintf(b, sizeof b, "slot %d %s (value %d, type %d)", k,
                          record == world::kNoTexture ? "no file" : "failed decode", m.slot_values[size_t(k)],
                          m.slot_types[size_t(k)]);
            return std::string(b);
          };
          if (m.effect.rfind("LAKE", 0) == 0) continue;  // drawn flat on purpose
          if (failed(m.albedo)) why[i] = slot(0, m.albedo);
          else if ((m.layers == world::Layers::kVertexBlend || m.layers == world::Layers::kSplat) && failed(m.layer_b))
            why[i] = slot(1, m.layer_b);
          else if (m.layers == world::Layers::kSplat && failed(m.splat))
            why[i] = slot(2, m.splat);
        }
        struct Group {
          double px = 0;
          size_t tris = 0;
          double sum[3] = {0, 0, 0};
          float ymin = 1e9f, ymax = -1e9f;
        };
        std::map<std::tuple<std::string, std::string, std::string>, Group> area;
        double total = 0;
        for (size_t t = 0; t < mesh.material.size(); ++t) {
          const std::string& w = why[mesh.material[t]];
          if (w.empty()) continue;
          double sx[3], sy[3];
          bool behind = false;
          for (int k = 0; k < 3 && !behind; ++k) {
            const auto& p = mesh.positions[t * 3 + size_t(k)];
            const float q[3] = {float(p[0] - ox), p[1], float(p[2] - oz)};
            float o[4];
            for (int r = 0; r < 4; ++r) o[r] = clip[r] * q[0] + clip[4 + r] * q[1] + clip[8 + r] * q[2] + clip[12 + r];
            if (o[3] < 0.1f) behind = true;
            sx[k] = std::clamp(double(o[0] / o[3]), -1.0, 1.0) * 0.5 * width;
            sy[k] = std::clamp(double(o[1] / o[3]), -1.0, 1.0) * 0.5 * height;
          }
          if (behind) continue;
          const double a = 0.5 * std::fabs((sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]));
          if (a <= 0) continue;
          const std::string model = t < mesh.model.size() && mesh.model[t] < colorado.models().size()
                                        ? colorado.models()[mesh.model[t]].name
                                        : std::string("?");
          auto& e = area[{model, mesh.materials[mesh.material[t]].effect, w}];
          e.px += a;
          ++e.tris;
          for (int k = 0; k < 3; ++k) {
            const auto& p = mesh.positions[t * 3 + size_t(k)];
            for (int c = 0; c < 3; ++c) e.sum[c] += p[size_t(c)] / 3.0;
            e.ymin = std::min(e.ymin, p[1]), e.ymax = std::max(e.ymax, p[1]);
          }
          total += a;
        }
        std::vector<std::pair<double, decltype(area)::const_iterator>> top;
        for (auto it = area.begin(); it != area.end(); ++it) top.push_back({it->second.px, it});
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::printf("texture census: %.0f px of fallback-textured triangles (%.1f%% of the frame), %zu groups\n", total,
                    100.0 * total / (double(width) * height), area.size());
        const auto ds = showroom.world_draw_stats();
        std::printf("  renderer fallbacks: %zu draws with a texture that did not upload, %zu with a layer set that did not\n",
                    ds.white, ds.layer_fallback);
        const auto vram = renderer.vram_stats();
        const auto wts = showroom.world_texture_stats();
        std::printf("  vram: %u blocks, %u allocs (%.1f/%.1f MiB); world textures: resident %zu (%.1f MiB), evictions %llu, reuploads %llu, pools %zu\n",
                    vram.block_count, vram.allocation_count,
                    double(vram.allocation_bytes) / (1024.0 * 1024.0), double(vram.block_bytes) / (1024.0 * 1024.0),
                    wts.resident_count, double(wts.resident_bytes) / (1024.0 * 1024.0),
                    (unsigned long long)wts.evictions, (unsigned long long)wts.reuploads, wts.descriptor_pools);
        for (size_t i = 0; i < top.size() && i < 20; ++i) {
          const auto& [key, v] = *top[i].second;
          std::printf("  %9.0f px %6zu tris  %s  %s  %s  at %.0f %.0f %.0f, y %.1f to %.1f\n", v.px, v.tris,
                      std::get<0>(key).c_str(), std::get<1>(key).c_str(), std::get<2>(key).c_str(),
                      v.sum[0] / double(v.tris), v.sum[1] / double(v.tris), v.sum[2] / double(v.tris), v.ymin, v.ymax);
        }
      };
      if (!contacts_log_path.empty()) {
        contact_log.f = std::fopen(contacts_log_path.string().c_str(), "w");
        if (!contact_log.f) std::fprintf(stderr, "carview: cannot write %s\n", contacts_log_path.string().c_str());
      }
      auto drawn_name = [&](uint32_t t) {
        const auto& mesh = streamer->mesh();
        std::string s = t < mesh.model.size() && mesh.model[t] < colorado.models().size()
                            ? colorado.models()[mesh.model[t]].name
                            : "?";
        if (mesh.has_render_data() && mesh.material[t] < mesh.materials.size())
          s += " " + mesh.materials[mesh.material[t]].effect;
        return s;
      };
      auto log_contacts = [&]() {
        if (!contact_log.f || !streamer || !streamer->uses_collision()) return;
        const auto& cm = streamer->collision_mesh();
        if (streamer->gathers() != contact_log.gathers) {
          contact_log.gathers = streamer->gathers();
          contact_log.drawn.build(streamer->mesh());
          contact_log.all.build(streamer->mesh(), false);
          {
            // The undecoded fields by surface and slope, over the whole gather.
            std::map<std::tuple<int, int, int, int>, size_t> kinds;  // surface, steep, word, edges class
            for (size_t t = 0; t < cm.triangles(); ++t) {
              const auto& a = cm.positions[t * 3];
              const auto& b = cm.positions[t * 3 + 1];
              const auto& cc = cm.positions[t * 3 + 2];
              const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
              const double vx = cc[0] - a[0], vy = cc[1] - a[1], vz = cc[2] - a[2];
              const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
              const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
              const int steep = len > 0 && std::fabs(ny / len) < 0.5;
              const int word = cm.word[t] == 0x8000 ? -1 : cm.word[t];
              const uint32_t e = cm.edges[t];
              const int edges = e == 0x00FFFFFFu ? 0 : ((e & 0xFF000000u) ? 2 : 1);
              ++kinds[{cm.surface[t], steep, word, edges}];
            }
            for (const auto& [k, n] : kinds)
              if (n >= 20 || std::get<1>(k))
                std::fprintf(contact_log.f, "  kind: surface %d (%s) %s word %s edges %s: %zu\n", std::get<0>(k),
                             collision.surface_name(std::get<0>(k)).c_str(), std::get<1>(k) ? "steep" : "flat",
                             std::get<2>(k) < 0 ? "0x8000" : std::to_string(std::get<2>(k)).c_str(),
                             std::get<3>(k) == 0 ? "0x00ffffff" : std::get<3>(k) == 1 ? "other" : "top byte set", n);
          }
          const auto census_t0 = std::chrono::steady_clock::now();
          size_t samples = 0, bare = 0;
          const auto groups = carview::undrawn_walls(cm, streamer->ground(), contact_log.drawn, streamer->centre_x(),
                                                     streamer->centre_z(), 200, 1.0, &samples, &bare);
          std::fprintf(contact_log.f, "gather %d at %.1f %.1f (t %.2f s): %zu drawn triangles indexed; walls within "
                       "200 m at car height: %zu samples (0.5 m), %zu with nothing drawn within 1 m, in %zu groups\n",
                       contact_log.gathers, streamer->centre_x(), streamer->centre_z(), vehicle.time(),
                       contact_log.drawn.triangles(), samples, bare, groups.size());
          std::printf("contacts log: gather %d census %.2f s\n", contact_log.gathers,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - census_t0).count());
          std::fflush(stdout);
          std::fflush(contact_log.f);
          for (size_t k = 0; k < groups.size() && k < 40; ++k) {
            const auto& g = groups[k];
            std::fprintf(contact_log.f, "  bare: tile %u surface %d (%s) flags 0x%02x word 0x%04x edges 0x%08x: %zu of %zu samples in %zu "
                         "triangles, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f\n",
                         g.tile, g.surface, collision.surface_name(g.surface).c_str(), g.flags, g.word, g.edges, g.bare, g.samples,
                         g.triangles, g.lo[0], g.hi[0], g.lo[1], g.hi[1], g.lo[2], g.hi[2]);
          }
        }
        const auto& c = vehicle.strongest_body_contact();
        const double hard = 0.5 * vehicle.params().mass * 9.81;
        if (c.force < hard) {
          contact_log.touching = false;
          return;
        }
        const drive::Vec3 dp = c.point - contact_log.last;
        if (contact_log.touching && std::sqrt(drive::dot(dp, dp)) < 3) return;
        contact_log.touching = true;
        contact_log.last = c.point;
        ++contact_log.contacts;
        const auto& src = streamer->ground_source();
        const int64_t ct = c.triangle >= 0 && size_t(c.triangle) < src.size() ? int64_t(src[size_t(c.triangle)]) : -1;
        const std::array<float, 3> p{float(c.point.x), float(c.point.y), float(c.point.z)};
        uint32_t near_t = 0;
        const double near1 = contact_log.drawn.nearest(p, 1.0);
        const double near5 = contact_log.drawn.nearest(p, 5.0, &near_t);
        if (near1 < 0) ++contact_log.undrawn;
        std::fprintf(contact_log.f, "contact t %.2f s at %.2f %.2f %.2f normal %.2f %.2f %.2f: %.0f N at %.0f km/h; "
                     "surface %d (%s)",
                     vehicle.time(), c.point.x, c.point.y, c.point.z, c.normal.x, c.normal.y, c.normal.z, c.force,
                     vehicle.speed() * 3.6, c.surface, collision.surface_name(c.surface).c_str());
        if (ct >= 0)
          std::fprintf(contact_log.f, " flags 0x%02x word 0x%04x edges 0x%08x tile %u triangle %lld", size_t(ct) < cm.flags.size() ? cm.flags[size_t(ct)] : 0, size_t(ct) < cm.word.size() ? cm.word[size_t(ct)] : 0, size_t(ct) < cm.edges.size() ? cm.edges[size_t(ct)] : 0u,
                       size_t(ct) < cm.tile.size() ? cm.tile[size_t(ct)] : 0u, (long long)ct);
        if (near1 >= 0) std::fprintf(contact_log.f, "; drawn %.2f m: %s\n", near1, drawn_name(near_t).c_str());
        else if (near5 >= 0) std::fprintf(contact_log.f, "; NOTHING DRAWN within 1 m (nearest %.2f m: %s)\n", near5, drawn_name(near_t).c_str());
        else std::fprintf(contact_log.f, "; NOTHING DRAWN within 5 m\n");
        if (ct >= 0) {
          const auto& q = cm.positions;
          const size_t b = size_t(ct) * 3;
          std::fprintf(contact_log.f, "    triangle: %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f\n", q[b][0],
                       q[b][1], q[b][2], q[b + 1][0], q[b + 1][1], q[b + 1][2], q[b + 2][0], q[b + 2][1], q[b + 2][2]);
        }
        // A profile across the contact along the wall's horizontal normal:
        // the collision's top surface and the drawn surfaces near the point's height.
        const double hn = std::hypot(c.normal.x, c.normal.z);
        if (hn > 0.3) {
          const double nx = c.normal.x / hn, nz = c.normal.z / hn;
          for (double s = -3; s <= 3.01; s += 0.5) {
            const double x = c.point.x + nx * s, z = c.point.z + nz * s;
            double y = 0;
            const bool ok = world::ground_height(*ground, x, z, c.point.y + 4, y);
            std::fprintf(contact_log.f, "    %+.1f m: collision %s", s, ok ? "" : "none");
            if (ok) std::fprintf(contact_log.f, "%.2f", y);
            std::fprintf(contact_log.f, "  drawn");
            int shown = 0;
            for (double h : contact_log.all.heights(x, z))
              if (h < c.point.y + 4 && h > c.point.y - 6 && shown++ < 4) std::fprintf(contact_log.f, " %.2f", h);
            std::fprintf(contact_log.f, "\n");
          }
        }
      };
      drive::Vec3 last_finite{};
      double last_finite_kmh = 0;
      // A line over the HUD for a few seconds (the time of day, a pose
      // saved, a collectible).
      std::string notice;
      double notice_until = -1;
      // The discovery card (game_hud.h), set with the drive time.
      struct Popup {
        std::string title, text;
        double at = -1e9, progress = -1;
        uint32_t colour = 0;
      } popup;
      bool save_due = new_career_save_pending;  // a collectible or fresh career: the window saves at once
      new_career_save_pending = false;
      bool skip_end_save = false;
      bool ui_atlas_synced = false;
      // The 1000 Club in this car: the gate's answer, the car's Data_Car id
      // and, once begun, one run per incomplete challenge (SPEC-V4-CONTENT,
      // Checking during driving).
      struct ClubRun {
        size_t index;
        freeroam::club::Run run;
        int failures_seen = 0;
      };
      std::vector<ClubRun> club_runs;
      int64_t club_car = -1;
      std::string club_gate;  // empty: open
      bool club_open = false;
      size_t club_pick = 0;
      SettingsPage settings_page;  // the pause menu's settings (M7)
      settings_page.set_available_luts(scan_lut_files(carview::settings_path().parent_path()));
      bool settings_live_overlay = false;  // true when opened via live F6 hotkey
      carview::Slice12Runtime slice12;
      slice12.save_tree_label = (slice12_modded_save || mods_session.isolation().isolated) ? "user-modded" : "user";
      LiveSettings boot_live_settings{};
      {
        SettingsPage boot_sp;
        boot_sp.load(read_settings(settings_path(), nullptr));
        boot_live_settings = boot_sp.live_settings();
        if (slice12_modded_save) boot_live_settings.trainer_cheats = true;
        slice12.trainer.config.cheats_enabled = boot_live_settings.trainer_cheats;
        if (boot_live_settings.trainer_saved_road.has_value())
          slice12.trainer.config.saved_map_road = boot_live_settings.trainer_saved_road;
        slice12.trainer.pacing = boot_live_settings.festival_pacing;
        slice12.trainer.hidden_cars = boot_live_settings.hidden_cars;
        slice12.wheel_shaping = boot_live_settings.wheel_shaping;
        slice12.ffb_cfg = boot_live_settings.wheel_ffb;
        slice12.ds_cfg = boot_live_settings.dualsense;
        slice12.ht_cfg = boot_live_settings.head_tracking;
        slice12.cam_editor_cfg = boot_live_settings.camera_editor;
        slice12.imm_cam_cfg = boot_live_settings.immersive_camera;
        slice12.tel_cfg = boot_live_settings.telemetry_udp;
      }
      // F6 > MODS (mods_page.h): opened from the settings overlay (M / X);
      // Profile > Save Backups & Save Sync (save_sync_ui.h): opened via P / Y.
      // F5 reloads replacement textures anywhere.
      carview::ModsPage mods_page;
      carview::SaveProfilePage save_page;
      save_page.set_media_resolver([&](int64_t id) {
        auto it = baked_ids.find(id);
        return it == baked_ids.end() ? std::string() : it->second;
      });
      auto reload_mod_textures = [&]() {
        size_t reuploaded = 0;
        const auto changed = mods_session.reload_textures(
            [&](uint32_t record, const cars::bake::Texture& t, bool srgb) {
              std::string why;
              return showroom.replace_world_texture(record, t, srgb, why);
            },
            &reuploaded);
        notice = changed.empty() ? "textures: no replacement changed"
                                 : "textures reloaded: " + std::to_string(changed.size()) + " changed, " +
                                       std::to_string(reuploaded) + " re-uploaded";
        notice_until = vehicle.time() + 4;
        mods_page.set_notice(notice);
        std::printf("mods: %s\n", notice.c_str());
      };
      freeroam::club::Signal club_signal;
      // Per-step game-event pulses accumulated during the physics step before club_step().
      struct ClubPendingPulses {
        std::vector<freeroam::club::SkillAward> skills;
        bool skill_chain_completed = false;
        std::vector<std::string> smashables;
        std::vector<freeroam::club::SpeedCameraHit> cameras;
        bool event_started = false;
        bool event_restarted = false;
        bool event_completed = false;
        int event_finish_place = 0;
        bool encounter_started = false;
        bool encounter_completed = false;
        int encounter_finish_place = 0;
        void clear() {
          skills.clear();
          skill_chain_completed = false;
          smashables.clear();
          cameras.clear();
          event_started = event_restarted = event_completed = false;
          event_finish_place = 0;
          encounter_started = encounter_completed = false;
          encounter_finish_place = 0;
        }
      } club_pending;
      carview::ClubMedalCeremony club_ceremony{};
      double club_ceremony_at = -1e9;
      bool club_race_was_started = false;
      bool club_race_was_finished = false;
      bool club_was_air = false;
      drive::Vec3 club_takeoff{};
      std::map<int64_t, std::string> club_event_names;
      std::map<int64_t, freeroam::club::CarMeta> club_car_meta;
      bool club_meta_built = false;
      auto club_ensure_meta = [&]() {
        if (!*car_names_loaded) carview::car_name("");  // ensures v4 carchallenge* string tables are loaded
        if (club_meta_built || !have_career_catalog) return;
        club_meta_built = true;
        for (const auto& cev : career_catalog.events) {
          std::string nm = carview::game_text(cev.name_ref);
          if (nm.empty() || nm.rfind("_&", 0) == 0) nm = cev.horizon_id;
          club_event_names[cev.event_id] = std::move(nm);
        }
        for (const auto& [cid, spec] : career_catalog.cars) {
          freeroam::club::CarMeta m;
          m.car_id = cid;
          m.base_cost = spec.base_cost;
          m.make = car_names->short_name(spec.media_name);
          club_car_meta[cid] = std::move(m);
        }
      };
      auto club_medal_stats = [&]() -> freeroam::club::ClubMedalStats {
        club_ensure_meta();
        std::vector<std::pair<int64_t, int>> per_car;
        per_car.reserve(saved.club.size());
        for (const auto& c : saved.club) {
          int n = 0;
          for (const auto& ch : c.challenges) n += ch.second ? 1 : 0;
          if (n > 0) per_car.emplace_back(c.car_id, n);
        }
        return freeroam::club::compute_medal_stats(per_car, club_car_meta);
      };
      auto club_record = [&]() -> drive::SavedClubCar* {
        for (auto& c : saved.club)
          if (c.car_id == club_car) return &c;
        return nullptr;
      };
      // Entry: the package owned, the green wristband (level index >= 1)
      // and a garage car (not a loaned or event car) with challenges.
      auto club_check = [&]() {
        club_runs.clear();
        club_car = -1;
        const char* state = std::getenv("SHIFT_STATE_ROOT");
        if (!club_tried) {
          club_tried = true;
          club_loaded = freeroam::club::load_club(state ? state : "", club_data, &club_why);
          if (club_loaded)
            std::printf("1000 club: %zu cars, %zu challenges (%zu measured), %zu locations\n",
                        club_data.cars.size(), club_data.challenges(), club_data.measured(),
                        club_data.locations.size());
        }
        if (!club_loaded) return club_gate = club_why, void();
        if (!colorado_world()) return club_gate = "the 1000 Club is in Colorado", void();
        bool owned = false;
        for (const auto& p : cars::list_packs(state ? state : ""))
          if (p.id == club_data.package) owned = p.owned();
        if (!owned) return club_gate = "the 1000 Club package is not enabled", void();
        if (wristbands.levels.empty() && !world_dir.empty()) {
          std::string e;
          race::load_wristband_tables(game_root_of(), state_root, shared_db, wristbands, &e);
        }
        if (wristbands.level_index(career.wristband_points) < 1 && !std::getenv("SHIFT_CLUB_ANY_BAND") && !demo_club)
          return club_gate = "needs the green wristband", void();
        // The car driven: the selected garage row when it is this car, else
        // any garage row of this car (a --car pick).
        if (have_garage) {
          auto is_this = [&](const garage::Car& gc) {
            auto it = baked_ids.find(gc.car_id);
            return it != baked_ids.end() && lower_ascii(it->second) == lower_ascii(car);
          };
          if (const garage::Car* gc = career_garage.selected_car(); gc && is_this(*gc)) club_car = gc->car_id;
          for (const auto& gc : career_garage.cars)
            if (club_car < 0 && is_this(gc)) club_car = gc.car_id;
        }
        if (club_car < 0 && demo_club && have_career_catalog) {
          if (const auto* sp = career_catalog.find_car_by_media(car)) club_car = sp->car_id;
        }
        if (club_car < 0) return club_gate = "only in your own garage cars", void();
        if (!club_data.cars.count(club_car)) return club_gate = "this car has no challenges", void();
        club_gate.clear();
        const auto& list = club_data.cars.at(club_car);
        drive::SavedClubCar* rec = club_record();
        if (!rec) return;  // not begun in this car
        if (rec->challenges.size() < list.size()) rec->challenges.resize(list.size(), {0u, false});
        if (rec->progress.size() < list.size()) rec->progress.resize(list.size());
        for (size_t i = 0; i < list.size(); ++i) {
          if (rec->challenges[i].first == 0u) rec->challenges[i].first = list[i].definition_hash;
          if (!rec->challenges[i].second) {
            freeroam::club::Run r(list[i], club_data.locations);
            if (!rec->progress[i].empty()) r.import_save_progress(rec->progress[i]);
            club_runs.push_back({i, std::move(r), 0});
          }
        }
      };
      // One step of every running challenge on the car's state and per-step game events.
      auto club_step = [&]() {
        if (club_runs.empty()) {
          club_pending.clear();
          return;
        }
        auto& s = club_signal;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 vel = vehicle.velocity();
        const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
        const drive::Vec3 right = drive::rotate(vehicle.orientation(), {1, 0, 0});
        const double v_fwd = vel.x * fwd.x + vel.y * fwd.y + vel.z * fwd.z;
        const double v_lat = std::fabs(vel.x * right.x + vel.y * right.y + vel.z * right.z);
        s.time = vehicle.time();
        s.speed_mph = std::fabs(vehicle.speed()) * 2.2369363;
        s.gear = vehicle.gear() < 0 ? 0 : vehicle.gear() == 0 ? -1 : vehicle.gear();
        bool air = true;
        for (const auto& w : vehicle.wheels()) air = air && !w.contact;
        air = air && vehicle.body_contact_force() <= 0;
        if (air && !club_was_air) club_takeoff = at;
        club_was_air = air;
        s.in_air = air;
        s.is_drifting = !air && s.speed_mph >= 12.0 && v_fwd > 2.0 && v_lat > 2.2 && (v_lat / std::max(1.0, v_fwd)) > 0.18;
        s.jump_m = air ? std::hypot(at.x - club_takeoff.x, at.z - club_takeoff.z) : 0;
        s.collided = vehicle.strongest_body_contact().force > 4 * vehicle.params().mass * 9.81 ||
                     (traffic && traffic->player_impulse() > 2.0 * vehicle.params().mass);
        s.x = at.x, s.z = at.z;

        // Skills, smashables, speed cameras, events, and H2H pulses.
        s.skills_awarded = std::move(club_pending.skills);
        s.skill_chain_completed = club_pending.skill_chain_completed;
        if (skill_tracker) {
          const auto sh = skill_tracker->hud();
          s.skill_points = sh.active ? double(sh.chain_points) * double(std::max(1, sh.multiplier)) : 0.0;
        } else {
          s.skill_points = 0.0;
        }
        s.smashables_broken = std::move(club_pending.smashables);
        s.speed_cameras = std::move(club_pending.cameras);
        s.event_started = club_pending.event_started;
        s.event_restarted = club_pending.event_restarted;
        s.event_completed = club_pending.event_completed;
        s.event_finish_place = club_pending.event_finish_place;
        s.encounter_started = club_pending.encounter_started;
        s.encounter_completed = club_pending.encounter_completed;
        s.encounter_finish_place = club_pending.encounter_finish_place;
        if (race) {
          const auto& ev = race->event();
          s.event_id = ev.event_id;
          s.career_event_style =
              (ev.style == 2 || ev.career_type == 4) ? "street" : (ev.style == 1 ? "festival" : "none");
          s.event_hub.clear();
          if (have_career_catalog) {
            if (const auto* cev = career_catalog.find_event(ev.horizon_id); cev && cev->hub_id > 0) {
              std::string hname;
              if (const auto* hub = career_catalog.find_hub(cev->hub_id))
                hname = lower_ascii(carview::game_text(hub->name_ref));
              if (hname.find("gladstone") != std::string::npos || cev->hub_id == 2) s.event_hub = "gladstone";
              else if (hname.find("red") != std::string::npos || cev->hub_id == 3) s.event_hub = "redrock";
              else s.event_hub = "carson";
            }
          }
          const auto& pc = race->session().car(race->player_index());
          // Before rolling off the start line, race_progress stays < 0 so wait-at-start challenges work cleanly.
          const double raw_prog = race->session().started() ? (pc.distance > 0.5 ? pc.distance : -0.01) : -0.01;
          s.race_progress = (raw_prog < 0.0 || std::fabs(vehicle.speed()) < 1.5 && pc.distance < 1.0) ? -0.01 : raw_prog;
        } else {
          s.event_id = 0;
          s.event_hub.clear();
          s.career_event_style = "none";
          s.race_progress = -1.0;
        }
        club_pending.clear();

        club_ensure_meta();
        const fh1::StringTables* stbls = car_names->strings();
        const auto& list = club_data.cars.at(club_car);
        drive::SavedClubCar* rec = club_record();
        for (size_t k = 0; k < club_runs.size();) {
          ClubRun& r = club_runs[k];
          const std::string title = list[r.index].format_title(stbls);
          const std::string desc = list[r.index].format_description(stbls, &club_event_names);
          const bool won = r.run.step(s);
          // Sync save_progress partial counts into the save record whenever they advance.
          if (rec && r.run.has_save_progress() && r.index < rec->progress.size()) {
            auto cur_prog = r.run.export_save_progress();
            if (cur_prog != rec->progress[r.index]) {
              rec->progress[r.index] = std::move(cur_prog);
              save_due = true;
            }
          }
          if (won) {
            if (rec) {
              rec->challenges[r.index].first =
                  rec->challenges[r.index].first ? rec->challenges[r.index].first : list[r.index].definition_hash;
              rec->challenges[r.index].second = true;
            }
            int done = 0;
            if (rec)
              for (const auto& c : rec->challenges) done += c.second;
            const auto mstats = club_medal_stats();
            std::printf("t %.2f s: 1000 club medal! %s (%d/%zu in this car, %d total)\n", s.time, title.c_str(), done,
                        list.size(), mstats.total_medals);
            club_ceremony = {};
            club_ceremony.active = true;
            club_ceremony.challenge_title = title;
            club_ceremony.challenge_desc = desc;
            club_ceremony.car_name = carview::car_name(car);
            club_ceremony.car_medals = done;
            club_ceremony.car_total = int(list.size());
            club_ceremony.total_medals = mstats.total_medals;
            club_ceremony.cars_with_medal = mstats.cars_with_medal;
            club_ceremony.age = 0.0;
            club_ceremony_at = s.time;
            audio_host.trigger_ui(audio::UiCue::kSignSmash);
            save_due = true;
            club_runs.erase(club_runs.begin() + std::ptrdiff_t(k));
            continue;
          }
          if (r.run.failures() != r.failures_seen) {
            r.failures_seen = r.run.failures();
            notice = "1000 CLUB: " + title + " FAILED - TRY AGAIN";
            notice_until = s.time + 3;
            std::printf("t %.2f s: %s\n", s.time, notice.c_str());
          }
          ++k;
        }
      };
      // Builds the full 1000 Club Pause Screen model with localized strings & live progress.
      auto build_club_screen = [&]() -> carview::ClubScreenState {
        club_ensure_meta();
        const fh1::StringTables* stbls = car_names->strings();
        carview::ClubScreenState cs;
        cs.active = true;
        cs.car_name = carview::car_name(car);
        cs.stats = club_medal_stats();
        if (!club_gate.empty() || club_car < 0 || !club_data.cars.count(club_car)) {
          cs.gate_reason = club_gate.empty() ? "1000 Club is unavailable for this car" : club_gate;
          return cs;
        }
        const auto& list = club_data.cars.at(club_car);
        const drive::SavedClubCar* rec = club_record();
        cs.challenges_begun = (rec != nullptr);
        cs.show_intro = (rec == nullptr || cs.stats.total_medals == 0);
        cs.car_total = int(list.size());
        cs.selected = int(std::min(club_pick, list.empty() ? size_t(0) : list.size() - 1));
        for (size_t i = 0; i < list.size(); ++i) {
          carview::ClubChallengeRow row;
          row.title = list[i].format_title(stbls);
          row.description = list[i].format_description(stbls, &club_event_names);
          row.hint = list[i].format_hint(0, stbls);
          row.completed = (rec && i < rec->challenges.size() && rec->challenges[i].second);
          if (row.completed) {
            ++cs.car_medals;
            row.progress = 1.0;
            row.status = "MEDAL EARNED";
          } else if (rec) {
            for (const auto& r : club_runs) {
              if (r.index == i) {
                row.status = r.run.status(club_signal);
                row.progress = r.run.progress(club_signal);
                row.has_save_progress = r.run.has_save_progress();
                break;
              }
            }
          } else {
            row.status = "READY";
          }
          cs.rows.push_back(std::move(row));
        }
        return cs;
      };
      // Text fallback for SHIFT_CLUB_PAGE debug logging.
      auto club_page = [&](std::string& title, std::vector<std::string>& lines) {
        lines.clear();
        const auto cs = build_club_screen();
        if (!cs.gate_reason.empty()) {
          title = "1000 club";
          lines.push_back(cs.gate_reason);
          return;
        }
        title = "1000 club: " + std::to_string(cs.car_medals) + "/" + std::to_string(cs.car_total) +
                " medals in this car";
        for (const auto& r : cs.rows) {
          lines.push_back(r.title + " - " + r.description + " [" + r.status + "]");
        }
        lines.push_back(cs.challenges_begun ? "challenges running in this car" : "begin challenges in this car");
      };
      // Active challenge strip while driving.
      auto build_club_active_strip = [&]() -> carview::ClubActiveStrip {
        carview::ClubActiveStrip st;
        if (club_runs.empty()) return st;
        club_ensure_meta();
        const fh1::StringTables* stbls = car_names->strings();
        const auto& list = club_data.cars.at(club_car);
        for (const auto& r : club_runs) {
          if (r.run.under_way(club_signal)) {
            st.active = true;
            st.title = list[r.index].format_title(stbls);
            st.status = r.run.status(club_signal);
            st.progress = r.run.progress(club_signal);
            return st;
          }
        }
        return st;
      };
      auto club_banner = [&]() -> std::string {
        const auto s = build_club_active_strip();
        return s.active ? ("1000 CLUB: " + s.title + " - " + s.status) : std::string();
      };
      // Translate GPS prompt events to Anna Sat-Nav VO cues.
      auto fire_satnav_event = [&](const traffic::GpsPromptEvent& gev) {
        using K = traffic::GpsPromptEvent::Kind;
        using T = traffic::GpsTurnKind;
        using D = traffic::GpsDestKind;
        if (gev.kind == K::kNone) return;
        audio::SatNavCue cue = audio::SatNavCue::kDestGeneric;
        bool play = true;
        switch (gev.kind) {
          case K::kDestinationSet:
            switch (gev.dest_kind) {
              case D::kFestival: cue = audio::SatNavCue::kDestFestival; break;
              case D::kFestivalRace: cue = audio::SatNavCue::kDestFestivalRace; break;
              case D::kShowcase: cue = audio::SatNavCue::kDestShowcase; break;
              case D::kOutpost: cue = audio::SatNavCue::kDestOutpost; break;
              case D::kWorkshop: cue = audio::SatNavCue::kDestWorkshop; break;
              case D::kAutoshow: cue = audio::SatNavCue::kDestAutoshow; break;
              case D::kPaintShop: cue = audio::SatNavCue::kDestPaintShop; break;
              case D::kRaceCentral: cue = audio::SatNavCue::kDestRaceCentral; break;
              case D::kStreetRace: cue = audio::SatNavCue::kDestStreetRace; break;
              default: cue = audio::SatNavCue::kDestGeneric; break;
            }
            break;
          case K::kRecalculating: cue = audio::SatNavCue::kRecalculating; break;
          case K::kUTurn: cue = audio::SatNavCue::kUTurn; break;
          case K::kArrivedNear: cue = audio::SatNavCue::kNearDestination; break;
          case K::kArrived:
            cue = audio::SatNavCue::kArrived;
            popup = {"DESTINATION REACHED", "YOU HAVE ARRIVED AT YOUR WAYPOINT", vehicle.time(), -1,
                     carview::ui::rgba(74, 238, 97)};
            break;
          case K::kTurnNow:
            switch (gev.turn) {
              case T::kLeft: cue = audio::SatNavCue::kTurnLeft; break;
              case T::kRight: cue = audio::SatNavCue::kTurnRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kSharpRight; break;
              default: play = false; break;
            }
            break;
          case K::kTurnNear:
            switch (gev.turn) {
              case T::kLeft: cue = audio::SatNavCue::kNearLeft; break;
              case T::kRight: cue = audio::SatNavCue::kNearRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kNearBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kNearBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kNearSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kNearSharpRight; break;
              default: play = false; break;
            }
            break;
          case K::kTurnFar:
            switch (gev.turn) {
              case T::kLeft: cue = audio::SatNavCue::kFarLeft; break;
              case T::kRight: cue = audio::SatNavCue::kFarRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kFarBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kFarBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kFarSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kFarSharpRight; break;
              default: play = false; break;
            }
            break;
          default: play = false; break;
        }
        if (play) audio_host.trigger_satnav(cue);
      };

      // Helper to launch an Outpost PR Stunt and route GPS to its target.
      bool pr_stunt_photo_pressed = false;
      bool pr_stunt_has_prev = false;
      double pr_stunt_prev_x = 0.0, pr_stunt_prev_z = 0.0;
      auto start_pr_stunt = [&](const freeroam::PrStunt& st) {
        int64_t base_pts = 0;
        if (skill_tracker) {
          const auto sh = skill_tracker->hud();
          base_pts = int64_t(std::llround(sh.popularity_points)) + int64_t(sh.chain_points) * int64_t(sh.multiplier);
        }
        pr_stunt_session.start(st, base_pts);
        pr_stunt_has_prev = false;
        pr_stunt_photo_pressed = false;
        const drive::Vec3 at = vehicle.position();
        if (gps_solver.ready()) {
          fire_satnav_event(gps_nav.set_destination(st.target_pos.x, st.target_pos.z, traffic::GpsDestKind::kGeneric,
                                                    st.title, at.x, at.z));
        }
        popup = {freeroam::pr_stunt_kind_name(st.kind), st.title, vehicle.time(), -1,
                 carview::ui::rgba(255, 146, 3)};
        audio_host.trigger_ui(audio::UiCue::kSkillAward);
      };

      // One free-roam step: the rules, then their notices and the props
      // they hide.
      auto free_roam_step = [&]() -> bool {
        if (!race) {
          const drive::Vec3 at = vehicle.position();
          if (gps_nav.active()) {
            const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
            const double hdg = std::atan2(f.x, f.z);
            fire_satnav_event(gps_nav.update(at.x, at.z, hdg, std::fabs(vehicle.speed()), drive::Vehicle::kStep));
          }
          if (road_discovery.ready()) {
            for (const auto& rev : road_discovery.update(at.x, at.z)) {
              char rb[96];
              std::snprintf(rb, sizeof rb, "ROAD DISCOVERED  %d / %d", rev.discovered_count, rev.total_count);
              std::string rname = rev.road_name;
              for (char& ch : rname) ch = char(std::toupper(static_cast<unsigned char>(ch)));
              popup = {rb, rname, vehicle.time(), double(rev.discovered_count) / double(std::max(1, rev.total_count)),
                       carview::ui::rgba(74, 238, 97)};
              audio_host.trigger_ui(audio::UiCue::kSkillAward);
              save_due = true;
            }
          }
          if (have_outposts) {
            const int disc_idx = outpost_progress.check_discovery(outposts, at.x, at.z);
            if (disc_idx >= 0 && size_t(disc_idx) < outposts.outposts.size()) {
              const auto& op = outposts.outposts[size_t(disc_idx)];
              if (skill_tracker) skill_tracker->add_popularity(500.0);
              const int dc = outpost_progress.discovered_count(outposts);
              popup = {"HORIZON OUTPOST DISCOVERED",
                       op.display_name + "  •  FAST TRAVEL UNLOCKED (" + std::to_string(dc) + "/" +
                           std::to_string(outposts.outposts.size()) + ")",
                       vehicle.time(), double(dc) / double(std::max<size_t>(1, outposts.outposts.size())),
                       carview::ui::rgba(255, 146, 3)};
              audio_host.trigger_ui(audio::UiCue::kSkillAward);
              save_due = true;
            }
            if (pr_stunt_session.active()) {
              freeroam::PrStuntUpdate psu;
              psu.car_x = at.x;
              psu.car_z = at.z;
              psu.prev_x = pr_stunt_has_prev ? pr_stunt_prev_x : at.x;
              psu.prev_z = pr_stunt_has_prev ? pr_stunt_prev_z : at.z;
              psu.speed_mph = std::fabs(vehicle.speed()) * 2.2369363;
              if (skill_tracker) {
                const auto sh = skill_tracker->hud();
                psu.total_skill_points =
                    int64_t(std::llround(sh.popularity_points)) + int64_t(sh.chain_points) * int64_t(sh.multiplier);
              }
              psu.photo_pressed = pr_stunt_photo_pressed;
              psu.dt = drive::Vehicle::kStep;
              pr_stunt_photo_pressed = false;
              pr_stunt_prev_x = at.x;
              pr_stunt_prev_z = at.z;
              pr_stunt_has_prev = true;
              const auto st_state = pr_stunt_session.update(psu);
              if (st_state == freeroam::PrStuntState::kSucceeded) {
                const auto& st = pr_stunt_session.stunt();
                const bool first_time = outpost_progress.complete_stunt(st.id);
                const int64_t raw_cr = first_time ? st.reward_credits : st.reward_credits / 4;
                const int64_t cr = slice12.trainer.pacing.scale_credits(raw_cr);
                const int64_t pop = first_time ? st.reward_popularity : st.reward_popularity / 4;
                career.credits += cr;
                career.total_winnings += cr;
                if (skill_tracker) skill_tracker->add_popularity(double(pop));
                popup = {"PR STUNT COMPLETE!",
                         st.title + "   +" + carview::thousands(cr) + " Cr   +" + carview::thousands(pop) + " POP",
                         vehicle.time(),
                         double(outpost_progress.completed_stunt_count(outposts)) /
                             double(std::max<size_t>(1, outposts.stunts.size())),
                         carview::ui::rgba(74, 238, 97)};
                audio_host.trigger_ui(audio::UiCue::kSkillAward);
                gps_nav.clear();
                save_due = true;
              } else if (st_state == freeroam::PrStuntState::kFailed) {
                popup = {"PR STUNT FAILED", "TIME EXPIRED  •  RETRY AT THE HORIZON OUTPOST", vehicle.time(), -1,
                         carview::ui::rgba(255, 72, 72)};
                gps_nav.clear();
              }
            }
          }
        }
        if (!free_roam) return false;
        freeroam::CarState cs;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
        const double fl = std::hypot(f.x, f.z);
        cs.x = at.x, cs.y = at.y, cs.z = at.z;
        if (fl > 1e-6) cs.forward_x = f.x / fl, cs.forward_z = f.z / fl;
        cs.half_width = vehicle.params().size.x / 2;
        cs.half_length = vehicle.params().size.z / 2;
        cs.speed = vehicle.speed();
        cs.moved = std::fabs(vehicle.speed()) * drive::Vehicle::kStep;
        bool sign_smashed_now = false;
        for (const auto& ev : free_roam->step(cs)) {
          char line[160];
          if (ev.kind == freeroam::Event::kSignSmashed) {
            sign_smashed_now = true;
            audio_host.trigger_ui(audio::UiCue::kSignSmash);
            audio::CollisionHit bhit{};
            bhit.rel_speed_mps = float(std::fabs(vehicle.speed()));
            bhit.pos = {float(at.x), float(at.y), float(at.z)};
            bhit.is_break = true;
            audio_host.trigger_collision(bhit);
            const drive::Vec3 vel_s = vehicle.velocity();
            particle_sys.spawn_smashable_burst("flyer", {float(at.x), float(at.y + 0.7), float(at.z)},
                                               {float(vel_s.x), float(vel_s.y + 2.5), float(vel_s.z)});
            const auto& sign_at = collectibles.signs[size_t(ev.index)].at;
            smashables.spawn_flyer_shatter({float(sign_at.x), float(sign_at.y), float(sign_at.z)},
                                           {float(vel_s.x), float(vel_s.y), float(vel_s.z)});
            const int n = free_roam->signs_found();
            std::snprintf(line, sizeof line, "discount sign %d/%zu - upgrades %d percent off", n, collectibles.signs.size(),
                          freeroam::upgrade_discount(n));
            std::printf("t %.2f s: %s smashed (%s)\n", vehicle.time(), collectibles.signs[size_t(ev.index)].id.c_str(), line);
            popup = {"DISCOUNT SIGN " + std::to_string(n) + "/" + std::to_string(collectibles.signs.size()),
                     "UPGRADES " + std::to_string(freeroam::upgrade_discount(n)) + "% OFF", vehicle.time(),
                     double(n) / double(std::max<size_t>(1, collectibles.signs.size())), carview::ui::rgba(255, 186, 20)};
          } else if (ev.kind == freeroam::Event::kRumourStarted) {
            std::snprintf(line, sizeof line, "barn find rumour - M for the map");
            popup = {"BARN FIND RUMOUR", "A CLASSIC IS HIDING NEARBY.  M FOR THE MAP", vehicle.time(), -1,
                     carview::ui::rgba(70, 210, 100)};
            std::printf("t %.2f s: %s rumour at %.1f km driven\n", vehicle.time(),
                        collectibles.barns[size_t(ev.index)].name.c_str(), progress.total_distance / 1000);
          } else {
            const auto& b = collectibles.barns[size_t(ev.index)];
            if (!have_garage && have_career_catalog) {
              if (const auto* spec = career_catalog.find_car_by_media(car))
                award_garage_car(spec->car_id, 0, true);
            }
            const std::string awarded = award_garage_car(b.car_id, 0, false);
            garage_picker();
            // The car's name: the Barnfind table's IDS_ key, by the id rule.
            std::string named = barn_car_label(b);
            if (!b.car_name_key.empty()) {
              const uint32_t ref = uint32_t(fh1::string_hash("Barnfind")) << 16 | fh1::string_hash(b.car_name_key);
              const std::string t = carview::game_text("_&" + std::to_string(ref));
              if (!t.empty() && t[0] != '_') named = t;
            }
            if (named == barn_car_label(b) && !awarded.empty()) named = awarded;
            std::snprintf(line, sizeof line, "barn find! %s is in the garage", named.c_str());
            popup = {"BARN FIND!", named + " IS IN YOUR GARAGE", vehicle.time(), -1, carview::ui::rgba(70, 210, 100)};
            std::printf("t %.2f s: %s found: %s (Data_Car %d) to the garage\n", vehicle.time(), b.name.c_str(),
                        named.c_str(), b.car_id);
          }
          save_due = true;
        }
        if (streamer && free_roam->hidden() != hidden_sent) streamer->set_hidden(hidden_sent = free_roam->hidden());
        return sign_smashed_now;
      };
      int prev_race_place = 0;
      std::string won_car_name;
      bool have_touch = false;
      double last_touch_y = 0;  // where the wheels last touched, for the fall check
      bool have_prev_speed_pos = false;
      double prev_speed_x = 0.0, prev_speed_z = 0.0;
      bool demo_h2h_started = false, demo_speed_trap_done = false;
      carview::RigAccelState rig_accel;
      auto current_make_id = [&]() -> int64_t {
        if (have_career_catalog) {
          if (const auto* spec = career_catalog.find_car_by_media(car)) return int64_t(spec->props.make);
        }
        return 0;
      };
      double step_physics_ms = 0.0;
      double step_ai_traffic_ms = 0.0;
      double step_world_stream_ms = 0.0;
      auto step = [&](const drive::Input& user_in) {
        const auto ph_t0 = std::chrono::steady_clock::now();
        const drive::Input in = race ? race->player_input(vehicle, user_in, traffic.get()) : user_in;
        vehicle.step(in, *ground);
        if (race) race->replay_player(vehicle);
        {
          // A car that left the numbers (NaN) stops the run: a gather around
          // NaN takes the whole map.
          const drive::Vec3 at = vehicle.position();
          if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
            std::fprintf(stderr, "carview: t %.3f s: the car's state is not finite (last at %.2f %.2f %.2f, %.1f km/h)\n",
                         vehicle.time(), last_finite.x, last_finite.y, last_finite.z, last_finite_kmh);
            error = "the car's state is not finite";
            return false;
          }
          last_finite = at;
          last_finite_kmh = vehicle.speed() * 3.6;
        }
        log_contacts();
        const auto ai_t0 = std::chrono::steady_clock::now();
        step_physics_ms += std::chrono::duration<double, std::milli>(ai_t0 - ph_t0).count();
        if (traffic) traffic->step(vehicle);
        if (race && streamer) {
          race->after_step(vehicle, *ground, streamer->centre_x(), streamer->centre_z(),
                           streamer->collision_radius(), traffic.get());
          const bool r_started = race->session().started();
          if (r_started && !club_race_was_started) club_pending.event_started = true;
          club_race_was_started = r_started;
          const auto& pc = race->session().car(race->player_index());
          const bool r_finished = (pc.finish >= 0.0);
          if (r_finished && !club_race_was_finished) {
            club_pending.event_completed = true;
            club_pending.event_finish_place = pc.place;
          }
          club_race_was_finished = r_finished;
          if (race->is_stage()) {
            race::CoDriverAudioCue cd_cue;
            while (race->codriver().pop_audio_cue(cd_cue)) {
              audio_host.trigger_codriver(cd_cue.group + "/" + cd_cue.cue);
            }
          }
        } else {
          club_race_was_started = false;
          club_race_was_finished = false;
        }
        if (race && race->is_stage() && race->player_finished() && rally_session.active() && !rally_service_open &&
            !rally_results_open) {
          const double ftime = race->stage_finish_time();
          const auto fin = rally_session.record_stage_finish(rally_catalog, ftime, race->stage_field());
          audio_host.trigger_codriver(fin.stage_place <= 10 ? "CoDriver/FinishGood/Good1" : "CoDriver/FinishBad/Bad1");
          if (fin.championship_complete) {
            const int dpct = race::difficulty_bonus_percent(int(race_options.difficulty));
            const int apct = race::assist_difficulty_bonus_percent(race_options.assists);
            const auto rew = rally_session.finalize(rally_catalog, saved, dpct, apct);
            career.credits = saved.credits;
            career.total_winnings = saved.total_winnings;
            rally_results_state = carview::make_rally_results_state(rally_catalog, rally_session, rew, saved.credits);
            rally_results_open = true;
            save_due = true;
          } else {
            rally_service_open = true;
            rally_service_selected = 0;
            rally_service_repaired = false;
            rally_notice.clear();
          }
        }
        if (race && race->session().started()) {
          race_top_speed = std::max(race_top_speed, std::fabs(vehicle.speed()));
          race::RaceBonusInputs bonus_in;
          bonus_in.assists_pct = race::assist_difficulty_bonus_percent(race->assists());
          bonus_in.skills_points = skill_tracker ? skill_tracker->race_skills_banked() : 0;
          bonus_in.wristband_level = collected_wristband;
          if (const race::RaceAward* a = race->award(career, wristbands, race_top_speed, bonus_in)) {
            if (slice12.trainer.pacing.modifies_economy()) {
              const int64_t scaled_cr = slice12.trainer.pacing.scale_credits(a->credits);
              const int64_t extra_cr = scaled_cr - a->credits;
              if (extra_cr != 0) {
                career.credits = std::clamp<int64_t>(career.credits + extra_cr, 0, 999999999);
                career.total_winnings = std::max<int64_t>(0, career.total_winnings + extra_cr);
              }
              const int scaled_wb = slice12.trainer.pacing.scale_points(a->wristband_points);
              const int extra_wb = scaled_wb - a->wristband_points;
              if (extra_wb != 0) career.wristband_points = std::max(0, career.wristband_points + extra_wb);
            }
            if (skill_tracker) skill_tracker->reset_race_skills_banked();
            if (have_prog_tables) {
              const int drv_xp = slice12.trainer.pacing.scale_points(a->driver_xp);
              const int aff_xp = slice12.trainer.pacing.scale_points(a->affinity_xp);
              const auto pu = race::apply_progression_xp(career, drv_xp, current_make_id(), aff_xp,
                                                         prog_tables);
              for (int64_t gcid : pu.gift_cars) {
                const std::string gnm = award_garage_car(gcid, 0, false);
                if (!gnm.empty()) {
                  popup = {"DRIVER LEVEL " + std::to_string(pu.new_driver_level) + " GIFT CAR!", gnm, vehicle.time(),
                           -1, carview::ui::rgba(247, 255, 38)};
                  garage_picker();
                }
              }
              if (pu.affinity_bonus_credits > 0) {
                sponsor_banner_title = "AFFINITY LEVEL " + std::to_string(pu.new_affinity_level);
                sponsor_banner_detail = "+" + carview::thousands(pu.affinity_bonus_credits) + " Cr  (" +
                                        std::to_string(int(std::lround(pu.affinity_discount * 100.0))) + "% UPGRADE DISC)";
                sponsor_banner_at = vehicle.time();
              }
            }
            const int lvl = wristbands.level_index(career.wristband_points);
            std::printf("race: award %lld CR (base %lld, mileage %lld, diff +%d%% %lld, assists %lld, skills %lld, nemesis %lld), %d wristband points (total %d, %s), credits %lld%s%s\n",
                        (long long)a->credits, (long long)a->base_credits, (long long)a->mileage_bonus,
                        a->difficulty_pct, (long long)a->difficulty_bonus, (long long)a->assists_bonus,
                        (long long)a->skills_bonus, (long long)a->nemesis_bonus, a->wristband_points,
                        career.wristband_points, lvl >= 0 ? wristbands.levels[size_t(lvl)].second.c_str() : "no wristband",
                        (long long)career.credits, a->first_completion ? ", first completion" : "",
                        a->prize_car ? (", prize car " + std::to_string(a->prize_car)).c_str() : "");
            if (a->prize_car > 0) {
              int prize_col = 0;
              if (const auto* cev = career_catalog.find_event(race_event)) prize_col = cev->prize_car_colour;
              won_car_name = award_garage_car(a->prize_car, prize_col, false);
              garage_picker();
            }
            // Dynamic difficulty (AISkillAdjustments): update profile Main/AISkill
            // after finished races in STOCK mode (skipped in Fair / Hard+).
            const double pl_t = race->session().car(race->player_index()).finish;
            const double ai_ref = race->ai_reference_time();
            const race::AiMode eff_mode =
                race::is_pinyon_hard(race_options.difficulty, race_options.ai_mode)
                    ? race::AiMode::kHard
                    : (race_options.rubberband ? race_options.ai_mode : race::AiMode::kFair);
            dynamic_ai_skill = race::update_dynamic_ai_skill(
                dynamic_ai_skill, pl_t, ai_ref, career.num_races, eff_mode, race->event().skill_adjustments);
            save_due = true;  // the window saves the award at once
          }
        }
        const bool smashed_sign = free_roam_step();
        if (smashed_sign) club_pending.smashables.push_back("sign");
        if (free_roam && free_roam->hidden()) smashables.sync_barns_from_hidden(*free_roam->hidden());
        bool smashed_prop = smashed_sign;
        // FORMAT-PHYSICS-DEFINITIONS: step 384-body smashable prop pool & barn doors.
        if (!bench || bench_full_fx) {
          std::vector<world::SmashEvent> sm_events;
          smashables.step(vehicle, *ground, float(drive::Vehicle::kStep), sm_events);
          for (const auto& sev : sm_events) {
            smashed_prop = true;
            club_pending.smashables.push_back(
                freeroam::club::classify_smashable_type(sev.prop_stem, sev.object_type_index));
            particle_sys.spawn_smashable_burst(sev.prop_stem, sev.pos, sev.impulse_vel);
            audio::CollisionHit bhit{};
            bhit.rel_speed_mps = float(std::fabs(vehicle.speed()));
            bhit.pos = {sev.pos[0], sev.pos[1], sev.pos[2]};
            bhit.is_break = true;
            audio_host.trigger_collision(bhit);
            if (sev.object_type_index == 1) audio_host.trigger_ui(audio::UiCue::kSignSmash);
          }
        }
        // SPEC-VFX-AND-PARTICLES: step tyre smoke/dust/gravel/sparks/backfire,
        // world ambient emitters, and persistent/fading skidmarks & ruts.
        if (!bench || bench_full_fx) {
          const drive::Vec3 vpos = vehicle.position();
          const drive::Vec3 vvel = vehicle.velocity();
          const drive::Quat vrot = vehicle.orientation();
          const drive::Vec3 vfwd = drive::rotate(vrot, {0, 0, 1});
          const drive::Vec3 vright = drive::rotate(vrot, {1, 0, 0});
          const drive::Vec3 vup = drive::rotate(vrot, {0, 1, 0});
          const auto& surfs = collision.surfaces();

          world::VehicleVfxInput vin{};
          vin.car_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
          vin.car_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
          vin.car_forward = {float(vfwd.x), float(vfwd.y), float(vfwd.z)};
          vin.car_right = {float(vright.x), float(vright.y), float(vright.z)};
          vin.car_up = {float(vup.x), float(vup.y), float(vup.z)};
          const float half_len = float(std::max(1.4, vehicle.params().size.z * 0.5));
          const float half_w = float(std::max(0.6, vehicle.params().size.x * 0.5));
          const drive::Vec3 ex_l = vpos + drive::rotate(vrot, {-half_w * 0.42, -0.22, -half_len - 0.05});
          const drive::Vec3 ex_r = vpos + drive::rotate(vrot, {half_w * 0.42, -0.22, -half_len - 0.05});
          vin.exhaust_tips[0] = {float(ex_l.x), float(ex_l.y), float(ex_l.z)};
          vin.exhaust_tips[1] = {float(ex_r.x), float(ex_r.y), float(ex_r.z)};
          vin.exhaust_tip_count = 2;

          const float idle_r = float(std::max(600.0, vehicle.params().idle_rpm));
          const float red_r = float(std::max(idle_r + 1000.0, vehicle.params().redline_rpm));
          vin.norm_rpm = std::clamp((float(vehicle.rpm()) - idle_r) / (red_r - idle_r), 0.0f, 1.0f);
          vin.throttle = float(in.throttle);
          vin.upshift_event = (vehicle.gear() > last_vfx_gear && vehicle.gear() >= 2);
          last_vfx_gear = vehicle.gear();
          vin.backfire_level = std::clamp(audio_upgrades.exhaust, 0, 3);
          const uint32_t cur_bf = audio_host.telemetry().backfire_count;
          if (cur_bf > last_audio_backfire_count) {
            vin.audio_backfire_pulses = cur_bf - last_audio_backfire_count;
          }
          last_audio_backfire_count = cur_bf;

          for (int wi = 0; wi < drive::kWheels; ++wi) {
            const auto& w = vehicle.wheels()[size_t(wi)];
            auto& wv = vin.wheels[size_t(wi)];
            wv.grounded = w.contact;
            if (!w.contact) {
              skid_sys.break_wheel(size_t(wi));
              continue;
            }
            const double tr = vehicle.params().tyre_radius[size_t(wi)] > 0 ? vehicle.params().tyre_radius[size_t(wi)] : 0.33;
            const drive::Vec3 patch = vpos + drive::rotate(vrot, {w.centre.x, w.centre.y - tr, w.centre.z});
            const double cs = std::cos(w.steer), sn = std::sin(w.steer);
            const drive::Vec3 wr = drive::rotate(vrot, {cs, 0, -sn});
            const drive::Vec3 wf = drive::rotate(vrot, {sn, 0, cs});
            wv.contact_pos = {float(patch.x), float(patch.y), float(patch.z)};
            wv.surface_normal = {float(vup.x), float(vup.y), float(vup.z)};
            wv.wheel_right = {float(wr.x), float(wr.y), float(wr.z)};
            wv.wheel_forward = {float(wf.x), float(wf.y), float(wf.z)};
            wv.tyre_width_m = 0.25f;
            wv.slip_combined = float(std::hypot(w.slip_ratio, w.slip_angle));
            wv.wheel_speed_mps = float(std::max(std::fabs(w.omega * tr), std::fabs(vehicle.speed())));
            wv.surface_index = w.surface;
            if (w.surface >= 0 && size_t(w.surface) < surfs.size()) {
              wv.surface = surfs[size_t(w.surface)];
            } else {
              // Synthetic fallback (asphalt) when driving on flat ground without collision surfaces.
              wv.surface.skid_a = 200;
              wv.surface.skid_texture_index = 0;
              wv.surface.skid_min_intensity = 0.0f;
              wv.surface.smoke_weight = 1.0f;
              wv.surface.should_spark = true;
            }
            skid_sys.update_wheel(size_t(wi), wv);
          }

          // Body scrape / impact sparks on ShouldSpark surfaces.
          const auto bc = vehicle.strongest_body_contact();
          const double bc_speed = bc.force * drive::Vehicle::kStep / std::max(500.0, vehicle.params().mass);
          if (bc.force > 1500.0) {
            bool spk = true;
            if (bc.surface >= 0 && size_t(bc.surface) < surfs.size()) spk = surfs[size_t(bc.surface)].should_spark;
            vin.scraping = true;
            vin.scrape_pos = {float(bc.point.x), float(bc.point.y), float(bc.point.z)};
            vin.scrape_normal = {float(bc.normal.x), float(bc.normal.y), float(bc.normal.z)};
            vin.scrape_should_spark = spk;
            if (bc_speed > 3.5 && vehicle.time() - last_impact_vfx_time > 0.25) {
              last_impact_vfx_time = vehicle.time();
              particle_sys.spawn_collision_impact(vin.scrape_pos, vin.scrape_normal, vin.car_vel, float(bc_speed), spk);
            }
          }

          particle_sys.update_vehicle(vin, float(drive::Vehicle::kStep));
          particle_sys.update_world_emitters(vin.car_pos, float(tod_clock.hour()), float(drive::Vehicle::kStep));
          particle_sys.step(float(drive::Vehicle::kStep), {0.707f, 0.0f, 0.707f}, 1.45f);
        }
        // SPEC-SKILLS-POPULARITY: step driving skill detectors in free roam and active races.
        if (skill_tracker && (!race || (race->session().started() && !race->player_finished()))) {
          freeroam::SkillInput si;
          si.dt = drive::Vehicle::kStep;
          si.time = vehicle.time();
          const drive::Vec3 vel = vehicle.velocity();
          const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
          const drive::Vec3 right = drive::rotate(vehicle.orientation(), {1, 0, 0});
          si.speed_ms = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
          si.forward_speed_ms = vel.x * fwd.x + vel.y * fwd.y + vel.z * fwd.z;
          si.lateral_speed_ms = vel.x * right.x + vel.y * right.y + vel.z * right.z;
          si.yaw_rate = vehicle.angular_velocity().y;
          si.handbrake = in.handbrake;
          if (have_career_catalog) {
            if (const auto* spec = career_catalog.find_car_by_media(car)) si.car_id = spec->car_id;
          }
          int on_ground = 0;
          double max_spin = 0;
          for (int wi = 0; wi < drive::kWheels; ++wi) {
            const auto& w = vehicle.wheels()[size_t(wi)];
            if (!w.contact) continue;
            ++on_ground;
            const double r = vehicle.params().tyre_radius[size_t(wi)] > 0 ? vehicle.params().tyre_radius[size_t(wi)] : 0.33;
            max_spin = std::max(max_spin, std::fabs(w.omega * r - si.forward_speed_ms));
          }
          si.wheels_on_ground = on_ground;
          si.wheel_spin = max_spin;
          const double mass = std::max(500.0, vehicle.params().mass);
          si.world_impact_ms = vehicle.strongest_body_contact().force * drive::Vehicle::kStep / mass;
          si.car_impact_ms = traffic ? traffic->player_impulse() / mass : 0.0;
          si.smashed_object = smashed_prop;
          const drive::Vec3 at = vehicle.position();
          auto check_other = [&](double ox, double oz, double ospeed) {
            const double dx = ox - at.x, dz = oz - at.z;
            const double d = std::hypot(dx, dz);
            si.nearest_car_dist = std::min(si.nearest_car_dist, d);
            const double ahead = dx * fwd.x + dz * fwd.z;
            const double side = std::fabs(dx * right.x + dz * right.z);
            if (ahead > 4.0 && ahead < 28.0 && side < 2.2 && ospeed > 15.0)
              si.draft_factor = std::max(si.draft_factor, std::clamp(1.0 - (ahead - 4.0) / 24.0, 0.0, 1.0));
          };
          if (traffic && traffic->cars())
            for (const auto& tc : *traffic->cars()) check_other(tc.position.x, tc.position.z, tc.speed);
          if (race) {
            std::vector<carview::RaceView::OpponentTag> otags;
            race->opponent_tags(otags);
            for (const auto& ot : otags) check_other(ot.x, ot.z, ot.speed);
            const int cur_place = race->session().car(race->player_index()).place;
            if (prev_race_place > 0 && cur_place < prev_race_place && race->session().clock() > 2.0) {
              si.passed_opponent = true;
              si.pass_speed_diff_mph = std::max(5.0, si.speed_ms * 0.25);
            }
            prev_race_place = cur_place;
          }
          const int64_t prev_max_skills = skill_tracker->max_skills_score();
          for (const auto& sev : skill_tracker->step(si)) {
            if (sev.kind == freeroam::SkillEvent::kSkillAwarded) {
              club_pending.skills.push_back(
                  {sev.skill_id, freeroam::club::skill_tracker_grade_to_club_grade(sev.grade)});
            } else if (sev.kind == freeroam::SkillEvent::kChainBanked ||
                       sev.kind == freeroam::SkillEvent::kChainLost) {
              club_pending.skill_chain_completed = true;
            }
            if (sev.kind == freeroam::SkillEvent::kFameSponsor && sev.credits > 0) {
              const int64_t scr = slice12.trainer.pacing.scale_credits(sev.credits);
              career.credits = std::min<int64_t>(999999999, career.credits + scr);
              career.total_winnings += scr;
              sponsor_banner_title = "FAME SPONSORSHIP TIER";
              sponsor_banner_detail = "RANK " + std::to_string(sev.rank) + "  •  +" + carview::thousands(scr) + " Cr";
              sponsor_banner_at = vehicle.time();
              popup = {"FAME SPONSOR BONUS",
                       "POPULARITY RANK " + std::to_string(sev.rank) + "  +" + carview::thousands(scr) + " Cr",
                       vehicle.time(), -1, carview::ui::rgba(247, 255, 38)};
              save_due = true;
            } else if (sev.kind == freeroam::SkillEvent::kSponsorChallenge && sev.credits > 0) {
              const int64_t scr = slice12.trainer.pacing.scale_credits(sev.credits);
              career.credits = std::min<int64_t>(999999999, career.credits + scr);
              career.total_winnings += scr;
              sponsor_banner_title = "SPONSORSHIP TIER " + std::to_string(sev.rank) + ": " + sev.skill_id;
              sponsor_banner_detail = "+" + carview::thousands(scr) + " Cr REWARD";
              sponsor_banner_at = vehicle.time();
              save_due = true;
            } else if (sev.kind == freeroam::SkillEvent::kRankUp) {
              // Notify if a Showcase event unlocked at this rank!
              if (have_career_catalog) {
                for (const auto& cev : career_catalog.events)
                  if (cev.style == 3 && cev.popularity_rank_req == sev.rank) {
                    const std::string ename = carview::game_text(cev.name_ref);
                    popup = {"NEW SHOWCASE UNLOCKED!",
                             (!ename.empty() && ename.rfind("_&", 0) != 0 ? ename : cev.horizon_id), vehicle.time(), -1,
                             carview::ui::rgba(245, 4, 92)};
                  }
              }
              save_due = true;
            }
          }
          if (skill_tracker->max_skills_score() > prev_max_skills) {
            int64_t cid = skill_tracker->max_skills_car();
            if (cid <= 0 && pre_loaner_car.empty() && have_garage) {
              if (const garage::Car* sc = career_garage.selected_car()) cid = sc->car_id;
            }
            if (cid <= 0 && have_career_catalog) {
              if (const auto* spec = career_catalog.find_car_by_media(car)) cid = spec->car_id;
            }
            if (cid > 0) max_skills_car = cid;
          }
        }
        // Speed Traps & Speed Zones (free-roam gate crossings, camera flash, audio cue, PB persistence).
        if (!race && speed_tracker) {
          const drive::Vec3 sp_at = vehicle.position();
          if (!have_prev_speed_pos) {
            prev_speed_x = sp_at.x;
            prev_speed_z = sp_at.z;
            have_prev_speed_pos = true;
          }
          if (demo_speed_trap && !demo_speed_trap_done && vehicle.time() >= 4.0 && !speed_data.traps.empty()) {
            demo_speed_trap_done = true;
            const auto& g = speed_data.traps.front().gate;
            const double gx = g.right[0] - g.left[0], gz = g.right[2] - g.left[2];
            const double gl = std::max(1.0, std::hypot(gx, gz));
            const double nx = -gz / gl, nz = gx / gl;
            speed_tracker->step(drive::Vehicle::kStep, vehicle.time(), g.center[0] - nx * 4.0, g.center[2] - nz * 4.0,
                                g.center[0] + nx * 4.0, g.center[2] + nz * 4.0, 64.5);  // ~144.3 mph flash & PB
          }
          const auto speed_evs = speed_tracker->step(drive::Vehicle::kStep, vehicle.time(), prev_speed_x, prev_speed_z,
                                                     sp_at.x, sp_at.z, vehicle.speed());
          prev_speed_x = sp_at.x;
          prev_speed_z = sp_at.z;
          for (const auto& ev : speed_evs) {
            if (ev.kind == freeroam::SpeedActivityEvent::kTrapTriggered ||
                ev.kind == freeroam::SpeedActivityEvent::kZoneCompleted) {
              const bool is_zone = (ev.kind == freeroam::SpeedActivityEvent::kZoneCompleted);
              int sb_id = 0;
              if (is_zone) {
                if (const auto* z = speed_data.find_zone(ev.id)) sb_id = z->scoreboard_id;
              } else {
                if (const auto* t = speed_data.find_trap(ev.id)) sb_id = t->scoreboard_id;
              }
              club_pending.cameras.push_back({is_zone, sb_id, ev.id, ev.speed_mph});
              if (ev.is_new_pb && have_prog_tables) {
                race::apply_progression_xp(career, 750, current_make_id(), 350, prog_tables);
              }
              if (skill_tracker) skill_tracker->add_popularity(ev.is_new_pb ? 500.0 : 150.0);
              std::printf("speed activity: %s %.1f mph%s\n", ev.label.c_str(), ev.speed_mph,
                          ev.is_new_pb ? " (NEW PB!)" : "");
              save_due = true;
            }
          }
        }
        // Free-roam Festival AI Racers & instant Head-to-Head point-to-point race.
        if (!race && festival_mgr) {
          const drive::Vec3 fp = vehicle.position();
          const drive::Vec3 ff = drive::rotate(vehicle.orientation(), {0, 0, 1});
          const double fh = std::atan2(ff.x, ff.z);
          if (demo_h2h && !demo_h2h_started && vehicle.time() >= 3.0) {
            demo_h2h_started = true;
            const uint32_t fid = festival_mgr->spawn_ahead({fp.x, fp.y, fp.z}, fh, 18.0);
            if (fid && festival_mgr->start_head_to_head({fp.x, fp.y, fp.z}, fh, vehicle.time(), fid)) {
              club_pending.encounter_started = true;
            }
            if (have_prog_tables && career.driver_xp < 36000) {
              race::apply_progression_xp(career, 36000, current_make_id(), 18000, prog_tables);
            }
            sponsor_banner_title = "SPONSORSHIP TIER 2: NEAR MISS";
            sponsor_banner_detail = "+10,000 Cr  •  BONDURANT RACING SCHOOL";
            sponsor_banner_at = vehicle.time();
          }
          freeroam::HeadToHeadResult hres;
          if (festival_mgr->step(drive::Vehicle::kStep, vehicle.time(), {fp.x, fp.y, fp.z}, fh, vehicle.speed(),
                                 &hres)) {
            club_pending.encounter_completed = true;
            club_pending.encounter_finish_place = hres.won ? 1 : 2;
            ++h2h_races;
            if (hres.won) {
              ++h2h_wins;
              const int64_t hcr = slice12.trainer.pacing.scale_credits(hres.payout_credits);
              career.credits = std::min<int64_t>(999999999, career.credits + hcr);
              career.total_winnings += hcr;
            }
            if (skill_tracker) skill_tracker->add_popularity(double(hres.popularity_points));
            if (have_prog_tables) {
              const int hxp = slice12.trainer.pacing.scale_points(hres.driver_xp);
              race::apply_progression_xp(career, hxp, current_make_id(), hxp / 2, prog_tables);
            }
            std::printf("h2h: %s vs %s (+%lld Cr, +%d Fame)\n", hres.won ? "WON" : "LOST",
                        hres.opponent_name.c_str(), (long long)hres.payout_credits, hres.popularity_points);
            save_due = true;
          }
        }
        club_step();  // evaluates all active 1000 Club challenges on full step signals
        if (!streamer) return true;
        // Headless runs put a car that is stuck or rolled back on the
        // nearest road (the windowed mode has R).
        if (headless && !race && follower.stuck(vehicle, in)) {
          const drive::Vec3 at = vehicle.position();
          double away = 0;
          if (safe_reset_ok && safe_reset.place(*streamer, vehicle, &away)) {
            rig_accel.reset();
            std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away\n", vehicle.time(), at.x,
                        at.y, at.z, away);
          } else if (world::Spawn sp; world::recover(*streamer, vehicle, &sp)) {
            rig_accel.reset();
            std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away (nearest road "
                        "triangle)\n", vehicle.time(), at.x, at.y, at.z, sp.distance);
          }
        }
        // Off the collision (the .fiz has holes outside the drivable strips):
        // a car 20 m below where its wheels last touched goes back on the
        // road, in the window too (ours; the game keeps cars out with walls).
        {
          bool touching = false;
          for (const auto& w : vehicle.wheels()) touching |= w.contact;
          const drive::Vec3 p = vehicle.position();
          if (touching || !have_touch) last_touch_y = p.y, have_touch = true;
          else if (p.y < last_touch_y - 20) {
            double away = 0;
            world::Spawn sp;
            const bool placed = (safe_reset_ok && safe_reset.place(*streamer, vehicle, &away)) ||
                                (world::recover(*streamer, vehicle, &sp) && ((away = sp.distance), true));
            std::printf("t %.2f s: fell through the ground at %.1f %.1f %.1f (no collision there)%s\n", vehicle.time(),
                        p.x, p.y, p.z, placed ? ", put back on the road" : "");
            if (placed) {
              rig_accel.reset();
              notice = "off the world: put back on the road";
              notice_until = vehicle.time() + 3;
            }
            last_touch_y = vehicle.position().y;
          }
        }
        const auto st_t0 = std::chrono::steady_clock::now();
        step_ai_traffic_ms += std::chrono::duration<double, std::milli>(st_t0 - ai_t0).count();
        bool changed = false;
        const drive::Vec3 at = vehicle.position();
        if (!streamer->update(at.x, at.z, &changed, &error)) return false;
        if (changed) {
          using clock = std::chrono::steady_clock;
          const auto t0 = clock::now();
          ground = &streamer->ground();
          follower.set_ground(&streamer->ground());  // its road points: set_staged_world
          const auto t1 = clock::now();
          if (!set_staged_world()) return false;
          const auto t2 = clock::now();
          std::printf("t %.2f s: gathered again around %.1f %.1f (%zu triangles, %.2f s; on the step: ground "
                      "%.1f ms, scenery hand-over %.1f ms)\n",
                      vehicle.time(), streamer->centre_x(), streamer->centre_z(), streamer->mesh().triangles(),
                      streamer->last_gather_seconds(), std::chrono::duration<double, std::milli>(t1 - t0).count(),
                      std::chrono::duration<double, std::milli>(t2 - t1).count());
        }
        bool swapped = false;
        const auto p0 = std::chrono::steady_clock::now();
        showroom.poll_world(false, &swapped);
        if (swapped && streamer_async)
          std::printf("t %.2f s: scenery around %.1f %.1f uploaded (swap %.1f ms)\n", vehicle.time(),
                      showroom.world_origin_x(), showroom.world_origin_z(),
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count());
        step_world_stream_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st_t0).count();
        return true;
      };
      int last_lut_zone = -2;
      float last_lut_day = -1.0f, last_lut_night = -1.0f;
      double tod_frame_dt = 0.0;
      bool tod_cockpit_cam = false;
      bool live_motion_blur = motion_blur_flag >= 0 ? motion_blur_flag == 1 : !low_preset;
      bool live_dof = dof_flag >= 0 ? dof_flag == 1 : !low_preset;
      bool live_photo_through_walls = photo_through_walls_cli;
      bool live_photo_wider_limits = photo_wider_limits_cli;
      carview::PhotoModeController photo_mode;
      carview::HorizonPromoTracker promo_tracker;
      promo_tracker.load(have_saved ? saved.promo_cars : std::vector<int>{}, std::max(240, int(baked_cars.size())));
      const std::filesystem::path photos_dir = carview::default_photos_dir();
      const std::filesystem::path replays_dir = carview::default_replays_dir();
      const bool ach_isolated = slice12_modded_save || mods_session.isolation().isolated;
      const std::filesystem::path achievements_path = [&]() {
        const auto legacy = carview::default_achievements_path();
        if (!save_path.empty()) {
          const auto scoped = save_path.parent_path() / "achievements.txt";
          std::error_code ec;
          if (!ach_isolated && !std::filesystem::exists(scoped, ec) && !legacy.empty() &&
              std::filesystem::exists(legacy, ec) && scoped != legacy) {
            std::filesystem::create_directories(scoped.parent_path(), ec);
            std::filesystem::copy_file(legacy, scoped, std::filesystem::copy_options::skip_existing, ec);
          }
          return scoped;
        }
        return legacy;
      }();
      fh1::SpaDatabase spa_db;
      {
        std::string spa_note;
        if (fh1::load_spa_packages(game_root_of(), state_root, spa_db, language, &spa_note) && !headless)
          std::printf("achievements: %s\n", spa_note.c_str());
      }
      carview::AchievementsSystem achievements;
      achievements.init(spa_db);
      if (demo_achievements_png.empty()) achievements.load_state(achievements_path);
      achievements.set_unlock_sound_callback([&]() { audio_host.trigger_ui(audio::UiCue::kAchievementReward); });
      double next_achievement_eval = 0.0;
      drive::ReplayRecorder replay_rec;
      carview::ReplayViewer replay_viewer;
      {
        drive::ReplayMetadata rm;
        rm.event_name = race ? race->hud_info().event : "COLORADO FREE ROAM";
        rm.world_name = world_dir.empty() ? "Colorado" : world_dir.filename().string();
        rm.tod_sec = tod_clock.tod_sec;
        rm.sample_hz = 60.0;
        rm.is_race = race != nullptr;
        rm.roster.push_back({0u, int32_t(info.car_id), car, carview::car_name(car), true, false});
        replay_rec.begin(rm, drive::ReplayRecorder::kDefaultMaxFrames, !race);
      }
      int replay_last_lap = 1;
      bool replay_finish_marked = false;
      auto open_replay_viewer = [&](bool browser_if_empty) {
        if (replay_rec.frame_count() >= 2) {
          if (race) {
            const auto rh = race->hud_info();
            replay_rec.update_summary(rh.place, rh.cars, rh.best_lap, tod_clock.tod_sec);
          }
          replay_viewer.open(replay_rec.snapshot(), true);
        } else if (browser_if_empty) {
          replay_viewer.open_browser(replays_dir);
        } else {
          notice = "NO REPLAY RECORDED YET";
          notice_until = vehicle.time() + 2.5;
        }
      };
      enum class PendingShot { kNone, kF8, kF12, kPhotoMode };
      PendingShot pending_shot = PendingShot::kNone;
      std::string pending_f12_pose_name;
      auto gather_promo_candidates = [&]() {
        std::vector<carview::PromoCarCandidate> cands;
        const drive::Vec3 pat = vehicle.position();
        cands.push_back({int(info.car_id), carview::car_name(car), {pat.x, pat.y, pat.z}, 2.2, true});
        if (race) {
          std::vector<carview::RaceView::OpponentTag> otags;
          race->opponent_tags(otags);
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
      };
      world::PostZoneBlender zone_blender;
      drive::CarLampState player_lamps;
      drive::CockpitAnimState cockpit_anim;
      int light_override = 0;  // 0 auto, 1 low beam, 2 full beam, 3 fog
      showroom.set_wind(tod_system.post_config.wind_speed, tod_system.post_config.wind_amplitude);
      auto place_world = [&]() {
        if (!streamer) return;
        float m[16];
        scenery_transform(info, vehicle, showroom.world_origin_x(), showroom.world_origin_z(), m);
        showroom.set_world_transform(m);
        if (traffic) traffic->place(info, vehicle);
        else showroom.set_instances({});
        if (race) {
          race->set_tod_sec(float(tod_clock.tod_sec));
          std::vector<Showroom::Instance> more;
          race->instances(info, vehicle, more);
          showroom.add_instances(more);
        } else if (festival_mgr && !festival_mgr->cars().empty()) {
          // Render roaming Festival AI racers & active H2H opponent as guest Showrooms.
          float pm[16];
          drive::model_to_world(info, vehicle, pm);
          float world_to_player[16];
          invert_affine(pm, world_to_player);
          std::vector<Showroom::Instance> fmore;
          const bool night_lamps = drive::CarLampState::tod_requests_headlamps(tod_clock.tod_sec);
          for (const auto& fc : festival_mgr->cars()) {
            auto it = festival_guests.find(fc.spec.car_media);
            if (it == festival_guests.end() && festival_guests.size() < 6) {
              auto gs = std::make_unique<Showroom>(renderer);
              gs->set_guest();
              std::string ge;
              if (gs->load(bake_dir, fc.spec.car_media, ShowroomOptions{}, ge) && gs->upload(ge)) {
                gs->load_car_lamps(game_root_of(), fc.spec.car_media);
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
            inst.lamps = ls.vectors();
            inst.has_lamps = true;
            fmore.push_back(inst);
          }
          if (!fmore.empty()) showroom.add_instances(fmore);
        }
        {
          drive::LampInputs lin{};
          const float bg = brake_glow(vehicle);
          lin.brake_cmd = bg;
          lin.brake_pedal = bg;
          lin.gear = vehicle.gear();
          lin.tod_sec = tod_clock.tod_sec;
          lin.dt = float(tod_frame_dt > 0.0 ? tod_frame_dt : 1.0 / 60.0);
          lin.light_override = light_override;
          lin.snap_headlamps = headless;
          player_lamps.update(lin);
          showroom.set_lamp_state(player_lamps);
          const float road_steer = float(vehicle.wheels()[0].steer);
          const float spd = float(std::max(0.0, vehicle.speed()));
          cockpit_anim.update_steering(road_steer, spd, lin.dt);
          const float redline = float(std::max(vehicle.params().redline_rpm, 6000.0));
          cockpit_anim.update_gauges(spd, 95.0f, float(vehicle.rpm()), redline, 0.0f, 20.0f, 0.85f);
          showroom.set_cockpit_anim(cockpit_anim, tod_cockpit_cam);
        }
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(m, up_game, up);
        if (tod_system.loaded_tod) {
          const world::TodState tod = tod_system.evaluate(tod_clock);
          rotate_direction(m, tod.sun_dir.data(), sun);
          showroom.set_world_light(sun, up, 1.0f);
          showroom.set_shadow_direction(sun);
          const drive::Vec3 vpos = vehicle.position();
          showroom.set_tod(tod, tod_clock.moon_phase(), float(vehicle.time()), float(vpos.y));
          if (post_on) {
            const auto& cfg = tod_system.post_config;
            PostSettings ps = renderer.post_settings();
            const float dn = tod.day_night_post;
            for (int k = 0; k < 6; ++k) ps.hable[k] = cfg.hable_day[k] + dn * (cfg.hable_night[k] - cfg.hable_day[k]);
            ps.white = cfg.white_day + dn * (cfg.white_night - cfg.white_day);
            ps.desaturate = std::clamp(cfg.desat_amount * tod.desat_amount_mult, 0.0f, 1.5f);
            for (int k = 0; k < 3; ++k) {
              ps.luminance[k] = cfg.desat_luminosity[k];
              ps.balance[k] = cfg.desat_post_mod[k];
            }
            if (const char* b = std::getenv("SHIFT_BALANCE"))
              std::sscanf(b, "%f,%f,%f", &ps.balance[0], &ps.balance[1], &ps.balance[2]);
            const int zone_idx = tod_system.zones.find_zone(float(vpos.x), float(vpos.z));
            const float step_dt = float(tod_frame_dt > 0.0 ? tod_frame_dt : (headless ? 10.0 : 1.0 / 60.0));
            zone_blender.update(tod_system.zones, cfg, float(vpos.x), float(vpos.z), step_dt);
            const auto& bt = zone_blender.bloom;
            const auto& cs = zone_blender.color;
            const auto& vt = zone_blender.vig_top;
            const auto& vb = zone_blender.vig_bottom;
            ps.bloom = (bloom_flag >= 0 ? bloom_flag == 1 : !low_preset) && slice12.trainer.config.debug_bloom;
            ps.bloom_cutoff = std::max(0.05f, bt.cutoff * tod.bloom_cutoff_mult);
            ps.bloom_scale = std::max(0.0f, bt.scale * tod.bloom_scale_mult);
            ps.bloom_lower_grad = std::max(0.0f, bt.lower_grad * tod.bloom_lower_grad_mult);
            ps.bloom_upper_grad = std::max(0.05f, bt.upper_grad * tod.bloom_upper_grad_mult);
            for (int k = 0; k < 5; ++k) ps.bloom_weights[k] = bt.weights[k];
            ps.vignette_amount = std::clamp(tod.vignette_amount, 0.0f, 1.0f);
            ps.contrast = std::clamp(1.0f + cs.contrast, 0.5f, 1.8f);
            ps.brightness = std::clamp(cs.brightness, -0.35f, 0.35f);
            ps.paired_vignette = !tod_system.zones.vignette_templates.empty() && !low_preset;
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
            ps.vig_top_feather = std::clamp(1.0f / std::max(vt.power, 0.5f), 0.15f, 0.85f);
            ps.vig_bot_opacity = vb.color[3];
            ps.vig_bot_center[0] = 0.5f + vb.pos_x * 0.5f;
            ps.vig_bot_center[1] = 0.5f - vb.pos_y * 0.5f;
            ps.vig_bot_radius[0] = std::max(vb.scale_x, 0.25f);
            ps.vig_bot_radius[1] = std::max(vb.scale_y, 0.25f);
            ps.vig_bot_rot = vb.angle_deg * kDegToRad;
            ps.vig_bot_feather = std::clamp(1.0f / std::max(vb.power, 0.5f), 0.15f, 0.85f);
            // Optical post-FX: speed/camera radial motion blur, DoF, god rays & lens flare/dirt.
            const float mph = float(std::max(0.0, vehicle.speed()) * 2.23693629);
            const float mph_max = std::max(cfg.detail_blur_mph_max, 40.0f);
            const float spd_t = std::clamp((mph - 18.0f) / mph_max, 0.0f, 1.0f);
            ps.motion_blur = live_motion_blur && slice12.trainer.config.debug_motion_blur && (!bench || bench_full_fx);
            ps.motion_blur_strength = std::clamp(spd_t * std::max(cfg.motion_blur_amount, 0.55f), 0.0f, 0.95f);
            ps.motion_blur_center[0] = 0.5f;
            ps.motion_blur_center[1] = 0.44f;
            ps.motion_blur_car_protect = tod_cockpit_cam ? 2.2f : 6.8f;
            ps.dof = live_dof && slice12.trainer.config.debug_dof && (!bench || bench_full_fx);
            ps.dof_focal_distance = std::clamp(cfg.dof_focal_distance, 5.0f, 40.0f);
            ps.dof_focal_range = 42.0f;
            ps.dof_max_blur = std::clamp(cfg.dof_focal_max_blur > 0.05f ? cfg.dof_focal_max_blur * 0.55f : 0.35f,
                                         0.15f, 0.65f);
            ps.near_plane = 0.25f;
            ps.god_rays = !low_preset && (!bench || bench_full_fx);
            ps.lens_flare = !low_preset && (!bench || bench_full_fx);
            ps.god_ray_strength = 0.48f;
            ps.lens_flare_strength = 0.36f;
            ps.time_sec = float(vehicle.time());
            ps.lut_enabled = true;
            if (exposure_given > 0) {
              ps.auto_exposure = false;
              ps.exposure = float(exposure_given);
            } else {
              ps.auto_exposure = true;
              const bool cockpit = tod_cockpit_cam;
              const float* keys = cockpit ? cfg.cockpit_keys : cfg.exterior_keys;
              const float* evs = cockpit ? cfg.cockpit_ev : cfg.exterior_ev;
              for (int k = 0; k < 4; ++k) ps.exp_keys[k] = keys[k], ps.exp_ev[k] = evs[k];
              // Linear base exposure from TrackSettings.xml (filmicTone.exposure = 0.31),
              // calibrated with a 1.18x daytime match to FH1 freeroam.png while keeping
              // midnight deep navy via the 4-key adaptive EV curve in post.frag.
              const float day_lift = 1.0f + 0.18f * (1.0f - std::clamp(tod.day_night_post, 0.0f, 1.0f));
              ps.exposure = std::max(cfg.filmic_exposure, 0.05f) * day_lift;
              const float tau = std::max(cfg.tonemap_delay * 10.0f, 0.05f);
              ps.adapt_alpha = (headless || tod_frame_dt <= 0.0)
                                   ? 1.0f
                                   : float(1.0 - std::exp(-tod_frame_dt / double(tau)));
            }
            if (photo_mode.active()) {
              ps = photo_mode.effects().apply_to_post(ps);
            }
            renderer.set_post_settings(ps);
            if (!user_lut_active &&
                (zone_idx != last_lut_zone || std::fabs(tod.day_color_grade - last_lut_day) > 0.005f ||
                 std::fabs(tod.night_color_grade - last_lut_night) > 0.005f)) {
              last_lut_zone = zone_idx;
              last_lut_day = tod.day_color_grade;
              last_lut_night = tod.night_color_grade;
              const auto lut = world::ColorLut16::blend(tod_system.zones.day_lut_for(zone_idx),
                                                        tod_system.zones.night_lut_for(zone_idx),
                                                        tod.day_color_grade, tod.night_color_grade);
              renderer.set_post_lut(lut.rgba);
            }
          }
        } else {
          rotate_direction(m, kSunGame, sun);
          showroom.set_world_light(sun, up, 1.0f);
          if (have_shadow_light) {
            float towards[3];
            rotate_direction(m, shadow_light_game, towards);
            showroom.set_shadow_direction(towards);
          }
        }
        if (!bench || bench_full_fx) {
          std::vector<world::SkidVertex> asph_tris, rut_tris;
          skid_sys.build_geometry(asph_tris, rut_tris);
          showroom.set_skidmarks(asph_tris, rut_tris);
          std::vector<world::DebrisVertex> debris_tris;
          smashables.build_debris_mesh(debris_tris);
          if (race && race->is_showcase()) race->append_showcase_mesh(debris_tris);
          const drive::Vec3 vpos = vehicle.position();
          world_anim.build_mesh(vpos.x, vpos.y + 1.5, vpos.z, float(vehicle.time()), ground, debris_tris, !low_preset);
          showroom.set_debris_mesh(debris_tris);
          std::vector<world::ParticleSprite> p_alpha, p_add;
          particle_sys.build_sprites({float(vpos.x), float(vpos.y + 1.5), float(vpos.z)}, p_alpha, p_add);
          showroom.set_particles(p_alpha, p_add, particle_sys.used_textures(), 0.25f, tod_cockpit_cam);
        }
      };
      // Writes where the car stands (drive/save.h); the start is the last spawn.
      drive::ControlSettings live_controls;
      drive::CarDamageState car_damage;
      if (have_saved && saved.has_controls) live_controls = saved.controls;
      {
        SettingsPage init_settings;
        init_settings.load(read_settings(settings_path(), nullptr));
        const LiveSettings init_ls = init_settings.live_settings();
        if (!have_saved || !saved.has_controls) live_controls = init_ls.controls;
        car_damage.mode = drive::DamageMode(init_ls.damage_mode);
      }
      bool live_sound_enabled = true;
      bool live_radio_enabled = true;
      bool loaded_camera_effects = live_controls.camera_effects;
      auto save_now = [&](const char* why) {
        if (save_path.empty() || status != 0) return;
        drive::DriveSave s;
        s.car = !pre_loaner_car.empty() ? pre_loaner_car : car;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
        s.position = {at.x, at.y, at.z};
        s.heading = std::atan2(fwd.x, fwd.z);
        s.has_spawn = streamer != nullptr;
        s.spawn = {start.x, start.y, start.z};
        s.spawn_heading = start_heading;
        s.credits = career.credits, s.total_winnings = career.total_winnings;
        s.wristband_points = career.wristband_points;
        s.num_races = career.num_races, s.num_victories = career.num_victories, s.num_podiums = career.num_podiums;
        s.time_festival = career.time_in_festival, s.time_street = career.time_in_street;
        s.time_exhibition = career.time_in_exhibition, s.top_speed = career.top_speed;
        for (const auto& [id, place] : career.race_places) s.race_results.emplace_back(id, place);
        for (const auto& [id, place] : career.event_places) s.event_results.emplace_back(id, place);
        s.prize_cars = career.prize_cars;
        if (progress_loaded) {
          progress_to_save(collectibles, progress, s);
        } else if (have_saved) {  // no collectibles this run: keep what the save had
          s.total_distance = saved.total_distance;
          s.signs_found = saved.signs_found;
          s.barns = saved.barns;
          s.garage = saved.garage;
        }
        s.owned_garage = have_garage || (have_saved && saved.owned_garage);
        s.career_started = (have_saved && saved.career_started) || s.owned_garage || career.num_races > 0 || career.credits > 0;
        s.intro_completed = have_saved ? saved.intro_completed : true;
        s.tod_sec = tod_clock.tod_sec;
        s.collected_wristband = collected_wristband;
        s.popularity_points = skill_tracker ? skill_tracker->popularity_points() : (have_saved ? saved.popularity_points : 0.0);
        s.max_skills_score = skill_tracker ? skill_tracker->max_skills_score() : (have_saved ? saved.max_skills_score : 0);
        s.max_skills_car = skill_tracker ? max_skills_car : (have_saved ? saved.max_skills_car : 0);
        s.fame_rewards_claimed = skill_tracker ? skill_tracker->fame_rewards_claimed() : (have_saved ? saved.fame_rewards_claimed : 0);
        s.difficulty = int(race_options.difficulty);
        s.ai_skill = dynamic_ai_skill;
        s.ai_mode = int(race_options.ai_mode);
        audio_host.save_radio_state(s);
        s.treasure_map = treasure_map_setting;
        if (road_discovery.ready()) {
          s.road_discovery_nodes = road_discovery.node_count();
          s.road_discovery_words = road_discovery.words();
        } else if (have_saved) {
          s.road_discovery_nodes = saved.road_discovery_nodes;
          s.road_discovery_words = saved.road_discovery_words;
        }
        s.discovered_outposts.assign(outpost_progress.discovered_outposts.begin(),
                                     outpost_progress.discovered_outposts.end());
        s.completed_pr_stunts.assign(outpost_progress.completed_pr_stunts.begin(),
                                     outpost_progress.completed_pr_stunts.end());
        if (speed_tracker) {
          s.speed_traps = speed_tracker->save_traps();
          s.speed_zones = speed_tracker->save_zones();
        } else if (have_saved) {
          s.speed_traps = saved.speed_traps;
          s.speed_zones = saved.speed_zones;
        }
        if (skill_tracker) {
          for (const auto& [sid, prog] : skill_tracker->sponsor_progress())
            s.sponsors.push_back({sid, prog.count, prog.claimed_tiers});
        } else if (have_saved) {
          s.sponsors = saved.sponsors;
        }
        s.h2h_races = h2h_races;
        s.h2h_wins = h2h_wins;
        s.driver_xp = career.driver_xp;
        for (const auto& [mid, xp] : career.make_affinity_xp) s.make_affinity.emplace_back(mid, xp);
        s.promo_cars = promo_tracker.save_ids();
        if (have_saved) s.carry_from(saved);
        if (career.driver_level > 0) s.level = career.driver_level;
        s.club = saved.club;
        s.has_controls = true;
        s.controls = live_controls;
        std::string e;
        if (!drive::write_save(save_path, s, &e)) {
          std::fprintf(stderr, "carview: %s\n", e.c_str());
        } else {
          if (headless) std::printf("save (%s): %s at %.1f %.1f %.1f\n", why, save_path.string().c_str(), at.x, at.y, at.z);
          save_session.on_saved(vehicle.time());
        }
        saved = s;
        have_saved = true;
      };
      auto eval_achievements = [&](bool toasts) {
        drive::DriveSave snap = have_saved ? saved : drive::DriveSave{};
        snap.career_started = snap.career_started || have_garage || career.num_races > 0 || career.credits > 0;
        snap.collected_wristband = std::max(snap.collected_wristband, collected_wristband);
        snap.num_races = std::max(snap.num_races, career.num_races);
        snap.num_victories = std::max(snap.num_victories, career.num_victories);
        snap.num_podiums = std::max(snap.num_podiums, career.num_podiums);
        snap.total_winnings = std::max(snap.total_winnings, career.total_winnings);
        snap.top_speed = std::max({snap.top_speed, career.top_speed, vehicle.speed()});
        if (progress_loaded) progress_to_save(collectibles, progress, snap);
        if (skill_tracker) {
          snap.popularity_points = std::max(snap.popularity_points, skill_tracker->popularity_points());
          snap.max_skills_score = std::max(snap.max_skills_score, skill_tracker->max_skills_score());
        }
        snap.discovered_outposts.assign(outpost_progress.discovered_outposts.begin(),
                                        outpost_progress.discovered_outposts.end());
        snap.completed_pr_stunts.assign(outpost_progress.completed_pr_stunts.begin(),
                                        outpost_progress.completed_pr_stunts.end());
        snap.promo_cars = promo_tracker.save_ids();
        const int roads_cnt = road_discovery.ready() ? road_discovery.discovered_count() : 0;
        auto ags = carview::AchievementGameplayState::from_drive_save(snap, roads_cnt, h2h_wins);
        if (achievements.evaluate(ags, toasts) > 0 && demo_achievements_png.empty())
          achievements.save_state(achievements_path);
      };
      if (demo_achievements_png.empty()) eval_achievements(false);
      // The 1000 Club's gate for this car, and its runs when begun;
      // SHIFT_CLUB_BEGIN=1 (debug) begins them as the page's last line does.
      club_check();
      if (std::getenv("SHIFT_CLUB_BEGIN") && club_gate.empty() && !club_record()) {
        drive::SavedClubCar rec;
        rec.car_id = club_car;
        rec.challenges.assign(club_data.cars.at(club_car).size(), {0u, false});
        saved.club.push_back(rec);
        club_check();
      }
      if (headless || club_gate.empty())
        std::printf("1000 club: %s\n", !club_gate.empty() ? club_gate.c_str()
                                        : club_runs.empty() && !club_record() ? "not begun in this car"
                                                                              : (std::to_string(club_runs.size()) +
                                                                                 " challenges running in this car").c_str());
      if (std::getenv("SHIFT_CLUB_PAGE")) {  // debug: the page as the menu shows it
        std::string title;
        std::vector<std::string> lines;
        club_page(title, lines);
        std::printf("%s\n", title.c_str());
        for (const auto& l : lines) std::printf("  %s\n", l.c_str());
      }
      WheelPose poses[4];
      CameraPull camera_pull;
      // SPEC-CAMERAS' rig (the old chase when it cannot load, and for
      // --view front).
      drive::DriveCamera rig;
      bool have_rig = false;
      drive::GamepadRumble gamepad_rumble;
      bool rumble_enabled = live_controls.rumble_enabled;
      double rumble_scale = live_controls.rumble_scale;
      int prev_rumble_gear = vehicle.gear();
      double rig_min_distance = 1e9, rig_max_distance = 0;  // the chase eye from the CoM, over the run
      if (!world_dir.empty()) {
        std::string note;
        have_rig = load_camera_rig(game_root_of(), bake_dir, info, vehicle, rig, note, loaded_camera_effects);
        if (!have_rig) std::fprintf(stderr, "carview: note: the camera spec's rig is off: %s\n", note.c_str());
        else if (!note.empty()) std::fprintf(stderr, "carview: note: camera: %s\n", note.c_str());
        rig.set_mode(drive::CameraMode(std::clamp(camera_mode, 0, drive::kCameraModes - 1)));
        cars::Install rumble_install;
        const char* rumble_state = std::getenv("SHIFT_STATE_ROOT");
        std::string rumble_xml, rumble_err;
        drive::RumbleConfig rumble_cfg;
        if (cars::discover_install(game_root_of(), rumble_state ? rumble_state : "", "", rumble_install, nullptr) &&
            cars::load_rumble_xml(rumble_install, rumble_xml, &rumble_err) &&
            drive::parse_controller_rumble(rumble_xml, rumble_cfg, &rumble_err)) {
          gamepad_rumble.set_config(rumble_cfg);
        }
      }
      slice12.apply_live_settings(boot_live_settings, rig, settings_path().parent_path());
      auto audio_view = [&]() {
        return (have_rig && !front) ? carview::engine_audio_view(rig.mode())
                                    : (front ? audio::View::kFront : audio::View::kFollow);
      };
      drive::RewindBuffer rewind_buf;
      rewind_buf.set_enabled(live_controls.assists.rewind);
      rewind_buf.start_session(vehicle.time());
      apply_vehicle_assists(vehicle, live_controls.assists);
      race_options.assists = to_difficulty_assists(live_controls.assists);
      drive::BrakingTable braking_table = drive::BrakingTable::build(vehicle.params());
      drive::DrivingLineMode driving_line_mode =
          static_cast<drive::DrivingLineMode>(std::clamp(live_controls.assists.driving_line, 0, 2));
      int pending_shift = 0;
      std::vector<drive::DrivingLineSample> dl_samples;
      std::vector<drive::DrivingLineVertex> dl_verts;
      std::vector<carview::WorldVertex> overlay_verts;
      drive::CameraView last_cam_view{};
      bool have_cam_view = false;
      // The frame for the drive's camera: the rig, else the old chase.
      auto drive_frame = [&](float aspect, double dt) {
        if (have_rig && !front) {
          bool interior = false;
          double d = 0;
          ShowroomFrame f = rig_frame(rig, info, vehicle, ground, rig_accel, dt, aspect, renderer.reverse_z(), &interior,
                                      &d, &last_cam_view);
          have_cam_view = true;
          if (rig.mode() == drive::CameraMode::kFollowLow || rig.mode() == drive::CameraMode::kFollowHigh) {
            rig_min_distance = std::min(rig_min_distance, d);
            rig_max_distance = std::max(rig_max_distance, d);
          }
          // BumperHigh hides the car; Cockpit/Hood/Follow draw it.
          showroom.set_draw_car(rig.mode() != drive::CameraMode::kBumper);
          return f;
        }
        showroom.set_draw_car(true);
        return chase_frame(info, vehicle, aspect, front, ground, &camera_pull, dt, renderer.reverse_z());
      };
      // The car's name and the keys, over the HUD for the first seconds.
      constexpr double kIntroSeconds = 6;
      // (The display name is a string-table key: the media name is shown.)
      const std::string intro = carview::car_name(car) + " - [ ] pick a car  R back on the road  T time of day  Esc menu";
      // The HUD panel in the bottom-right corner, 16 px in (scaled).
      // The race line on the HUD (RaceView: the count, the lap, place and
      // time, the result).
      auto race_banner = [&]() -> std::string {
        if (!race) return {};
        return race->player_finished() && !headless ? race->banner() + "  A/Enter continue  Y restart" : race->banner();
      };
      int hud_height = 0;  // the panel's, for the radio popup above it
      // The upgrade shop (SPEC-GARAGE-UPGRADES) on the selected garage car;
      // the discount is the signs found.
      carview::UpgradeShop shop;
      // Paint Shop & Vinyl/Livery Editor (LiveryEditor: 102 Special Colors, 1,372 vinyl shapes, 1024^2 compositor, .liv).
      bool paint_open = false;
      carview::LiveryEditor livery_editor;
      std::filesystem::path livery_edit_path;
      std::vector<size_t> paint_order;
      size_t paint_pick = 0, paint_before = 0;
      carview::Autoshow autoshow;
      carview::GarageBrowser garage_browser;
      int64_t garage_sell_confirm_id = -1;
      carview::BarnFindCard barn_card;
      cars::PhysicsSettings hub_phys_settings;
      garage::PiConfig hub_pi_cfg = garage::synthetic_pi_config();
      if (const auto gr = game_root_of(); !gr.empty()) {
        const auto pzip = gr / "media" / "physics.zip";
        cars::load_physics_settings(pzip, hub_phys_settings, nullptr);
        garage::load_pi_config(pzip, hub_pi_cfg, nullptr);
      }
      carview::CareerPage career_page;
      carview::EventsPage events_page;
      carview::CeremonyHud ceremony;
      bool garage_open = false;
      size_t garage_pick = 0;
      bool grid_menu_open = race != nullptr && !headless && !bench && !race_options.autopilot;
      int grid_menu_selected = 0;
      double title_orbit_time = 0.0;
      // Active world trigger in free roam (nearest Festival site, Street Hub, or unlocked Event).
      carview::WorldTrigger active_trigger;
      carview::WorldMarkerHud world_marker;
      std::vector<carview::NameTagHud> name_tags;
      carview::MapOverlayHud map_overlay;
      std::vector<MapMarker> cached_map_markers;
      int map_selected_marker = -1;
      auto affinity_discount_pct = [&]() -> int {
        if (!have_prog_tables) return 0;
        const int64_t mid = current_make_id();
        const auto it = career.make_affinity_xp.find(mid);
        const int axp = it == career.make_affinity_xp.end() ? 0 : it->second;
        return int(std::lround(prog_tables.affinity_discount_for_xp(axp) * 100.0));
      };
      auto discount = [&]() {
        const int signs_disc = freeroam::upgrade_discount(free_roam ? free_roam->signs_found() : int(saved.signs_found.size()));
        return std::min(100, signs_disc + affinity_discount_pct());
      };
      auto current_car_info = [&](int64_t& out_car_id, int& out_class_id) {
        out_car_id = 0, out_class_id = 2;
        if (have_garage) {
          if (const garage::Car* sc = career_garage.selected_car()) {
            out_car_id = sc->car_id;
            out_class_id = class_bands.empty() ? sc->class_id : garage::class_of(sc->performance_index, class_bands);
            return;
          }
        }
        if (have_career_catalog) {
          if (const auto* spec = career_catalog.find_car_by_media(car)) {
            out_car_id = spec->car_id;
            out_class_id = class_bands.empty() ? spec->stock_class : garage::class_of(spec->stock_pi, class_bands);
          }
        }
      };
      bool legacy_hud_cleared = false;
      // The results' page (SPEC-UI-LAYOUTS c): 0 FINISHED banner, 1 CASH REWARD, 2 POINTS EARNED, 3 NEW CAR WON.
      int results_page = 0;
      bool results_table = false;  // the RACE RESULTS table over the banner (X opens, B closes)
      bool results_points = false;  // a wristband points page follows the cash reward
      // The minimap in the UI atlas, blended by the UI pass (lower left;
      // mm_size 0: none).
      std::array<float, 4> mm_uv{};
      float mm_x = 0, mm_y = 0, mm_size = 0;
      double points_at = -1;        // the points page's start (the drive clock)
      // Regional Horizon Outpost Hub menu (PR Stunts, Fast Travel, Change Car, Nearby Events).
      struct OutpostHubItem {
        enum Kind { kPrStunt, kGarage, kNearbyEvents, kFastTravelFestival, kFastTravelOutpost } kind = kPrStunt;
        std::string label;
        std::string ref_id;
        std::string detail_title;
        std::vector<std::string> detail_lines;
        double dest_x = 0.0, dest_z = 0.0;
      };
      bool outpost_hub_open = false;
      std::string outpost_hub_id;
      size_t outpost_hub_pick = 0;
      std::vector<OutpostHubItem> outpost_hub_items;
      auto rebuild_outpost_hub = [&]() {
        outpost_hub_items.clear();
        const freeroam::Outpost* op = have_outposts ? outposts.find_outpost(outpost_hub_id) : nullptr;
        if (!op) return;
        const drive::Vec3 at = vehicle.position();
        for (const freeroam::PrStunt* st : outposts.stunts_for_outpost(op->id)) {
          const bool done = outpost_progress.is_stunt_completed(st->id);
          OutpostHubItem item;
          item.kind = OutpostHubItem::kPrStunt;
          item.ref_id = st->id;
          item.label = std::string(freeroam::pr_stunt_kind_name(st->kind)) + ": " + st->title + (done ? "  [DONE]" : "");
          item.detail_title = st->title;
          item.detail_lines.push_back(std::string("TYPE: ") + freeroam::pr_stunt_kind_name(st->kind));
          item.detail_lines.push_back(st->instruction);
          if (st->kind == freeroam::PrStuntKind::kSpeed) {
            item.detail_lines.push_back("TARGET SPEED: " + std::to_string(int(std::lround(st->target_mph))) + " MPH");
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
                               "CARS AVAILABLE: " + std::to_string(baked_cars.size())};
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
        for (const auto& other : outposts.outposts) {
          if (other.id == op->id) continue;
          if (!outpost_progress.is_outpost_discovered(other.id)) continue;
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
      };
      auto open_outpost_hub = [&](const std::string& id) {
        if (!have_outposts) return;
        const freeroam::Outpost* op = outposts.find_outpost(id);
        if (!op) return;
        outpost_progress.discover_outpost(op->id);
        outpost_hub_id = op->id;
        outpost_hub_pick = 0;
        rebuild_outpost_hub();
        outpost_hub_open = true;
      };
      if (!cli_outpost_hub.empty() && have_outposts) open_outpost_hub(cli_outpost_hub);
      if (fh1::CjkFont::language_needs_cjk(language)) {
        auto cf = std::make_shared<fh1::CjkFont>();
        if (cf->load_for_language(game_root_of(), language, nullptr)) carview::ui::set_cjk_font(cf);
      }
      carview::RadioPopupGpuState radio_popup_gpu;
      auto set_hud = [&](int win_w, int win_h, const std::string& banner, const PauseMenu* menu = nullptr) {
        const carview::modern::HudLayout hl = carview::modern::hud_layout(
            float(win_w), float(win_h), active_graphics.hud_lock_16_9, active_graphics.hud_scale,
            active_graphics.safe_zone);
        renderer.set_hud_layout(hl);
        const int width = std::max(1, int(std::lround(hl.virtual_w)));
        const int height = std::max(1, int(std::lround(hl.virtual_h)));
        HudState st;
        st.banner = banner;
        std::string detail_title;
        std::vector<std::string> detail_lines;
        if (menu) menu->fill(st);
        if (events_page.is_open()) events_page.fill(st.menu_title, st.menu, st.menu_selected, detail_title, detail_lines);
        if (outpost_hub_open) {
          const freeroam::Outpost* op = have_outposts ? outposts.find_outpost(outpost_hub_id) : nullptr;
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
          st.menu_title = "GARAGE (" + std::to_string(baked_cars.size()) + " CARS): ENTER DRIVE   ESC BACK";
          st.menu.clear();
          const int n = int(baked_cars.size());
          const size_t gp = n > 0 ? std::min(garage_pick, size_t(n - 1)) : 0;
          const int first = std::clamp(int(gp) - 3, 0, std::max(0, n - 7));
          for (int i = first; i < std::min(n, first + 7); ++i) {
            std::string label = carview::car_name(baked_cars[size_t(i)]);
            if (have_garage && size_t(i) < pick_ids.size()) {
              if (const garage::Car* gc = career_garage.find(pick_ids[size_t(i)]); gc && !class_bands.empty()) {
                const std::string cls = class_bands[size_t(garage::class_of(gc->performance_index, class_bands))].name;
                label = "[" + cls + " " + std::to_string(garage::shown_index(gc->performance_index, class_bands)) + "]  " +
                        label + (gc->id == career_garage.selected ? "  (ACTIVE)" : "");
              }
            }
            st.menu.push_back(label);
          }
          st.menu_selected = n > 0 ? int(gp) - first : 0;
        }
        if (shop.is_open()) shop.fill(class_bands, discount(), career.credits, st.menu_title, st.menu, st.menu_selected);
        if (autoshow.is_open()) autoshow.fill(class_bands, career.credits, st.menu_title, st.menu, st.menu_selected);
        if (career_page.is_open()) career_page.fill(st.menu_title, st.menu, st.menu_selected);
        if (settings_page.is_open()) settings_page.fill(st.menu_title, st.menu, st.menu_selected);
        if (paint_open) {
          st.menu_title = "PAINT SHOP (FREE): LEFT/RIGHT PREVIEW   ENTER APPLY";
          st.menu.clear();
          const auto& rows = showroom.colours();
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
        d.kmh = st.kmh, d.rpm = st.rpm, d.redline_rpm = st.redline_rpm, d.gear = st.gear, d.abs = st.abs,
        d.tcs = vehicle.tcs_active() || vehicle.stm_active();
        d.time = show_title ? title_orbit_time : vehicle.time();
        d.detail_title = std::move(detail_title);
        d.detail_lines = std::move(detail_lines);
        // Title Screen HUD (when active and settings isn't open over it).
        carview::TitleScreenHud ts_hud;
        if (show_title && !settings_page.is_open()) {
          ts_hud.active = true;
          ts_hud.menu_phase = title_menu_phase;
          ts_hud.has_save = have_saved && (saved.career_started || saved.owned_garage || career.num_races > 0 || career.credits > 0);
          ts_hud.confirm_new_career = title_confirm_new;
          ts_hud.confirm_selected = title_confirm_selected;
          if (ts_hud.has_save)
            ts_hud.menu = {"CONTINUE CAREER", "NEW CAREER", "FREE ROAM", "SETTINGS", "QUIT"};
          else
            ts_hud.menu = {"NEW CAREER", "FREE ROAM", "SETTINGS", "QUIT"};
          ts_hud.selected = std::clamp(title_selected, 0, int(ts_hud.menu.size()) - 1);
          if (ts_hud.has_save) {
            const int li = std::clamp(collected_wristband, 0, std::max(0, int(wristbands.levels.size()) - 1));
            if (!wristbands.levels.empty())
              ts_hud.save_band_name = carview::game_text(wristbands.levels[size_t(li)].second) + " WRISTBAND";
            ts_hud.save_band_points = career.wristband_points;
            ts_hud.save_pop_rank = current_pop_rank();
            ts_hud.save_car_name = carview::car_name(car);
            if (const garage::Car* sc = career_garage.selected_car(); sc && !class_bands.empty()) {
              ts_hud.save_car_class = class_bands[size_t(garage::class_of(sc->performance_index, class_bands))].name;
              ts_hud.save_car_pi = garage::shown_index(sc->performance_index, class_bands);
            }
            ts_hud.save_credits = career.credits;
            ts_hud.save_races = career.num_races;
            ts_hud.save_wins = career.num_victories;
          }
          d.title_screen = &ts_hud;
        }
        if (ceremony.active) d.ceremony = &ceremony;
        if (map_overlay.active) d.map_overlay = &map_overlay;
        carview::SkillChainHud sk_hud;
        if (skill_tracker) {
          const auto sh = skill_tracker->hud();
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
        if (world_marker.visible && !race) d.world_marker = &world_marker;
        carview::GpsHud gps_hud;
        if (!race && gps_nav.active()) {
          const auto& gg = gps_nav.guidance();
          gps_hud.active = true;
          gps_hud.turn_kind = int(gg.next_turn);
          gps_hud.turn_text = traffic::gps_turn_label(gg.next_turn);
          gps_hud.dest_label = gg.dest_label;
          gps_hud.dist_to_turn_m = gg.dist_to_turn_m;
          gps_hud.dist_to_dest_m = gg.dist_to_dest_m;
          d.gps = &gps_hud;
        }
        carview::PrStuntHud pr_hud;
        if (!race && pr_stunt_session.active()) {
          const auto& pst = pr_stunt_session.stunt();
          pr_hud.active = true;
          pr_hud.kind_badge = freeroam::pr_stunt_kind_name(pst.kind);
          pr_hud.title = pst.title;
          pr_hud.instruction = pst.instruction;
          pr_hud.status_line = pr_stunt_session.status_line();
          pr_hud.time_remaining_s = pr_stunt_session.time_remaining_s();
          pr_hud.progress = pr_stunt_session.progress_fraction();
          pr_hud.in_zone = pr_stunt_session.in_target_zone();
          pr_hud.is_photo = pst.kind == freeroam::PrStuntKind::kPhoto;
          d.pr_stunt = &pr_hud;
        }
        freeroam::SpeedActivityHud speed_hud_state;
        if (!race && speed_tracker) {
          speed_hud_state = speed_tracker->hud(vehicle.time());
          d.speed_hud = &speed_hud_state;
        }
        freeroam::HeadToHeadHud h2h_hud_state;
        if (!race && festival_mgr) {
          h2h_hud_state = festival_mgr->hud(vehicle.time());
          d.h2h = &h2h_hud_state;
        }
        d.driver_level = std::max(1, career.driver_level);
        d.driver_xp = career.driver_xp;
        if (have_prog_tables) {
          d.driver_next_xp = prog_tables.next_driver_level_xp(career.driver_xp);
          const int64_t mid = current_make_id();
          const auto ait = career.make_affinity_xp.find(mid);
          const int axp = ait == career.make_affinity_xp.end() ? 0 : ait->second;
          d.affinity_level = prog_tables.affinity_level_for_xp(axp);
          d.affinity_discount_pct = prog_tables.affinity_discount_for_xp(axp) * 100.0;
        }
        d.sponsor_title = sponsor_banner_title;
        d.sponsor_detail = sponsor_banner_detail;
        d.sponsor_at = sponsor_banner_at;
        d.name_tags = name_tags;
        carview::RaceHud race_hud;
        const bool race_line = race && banner == race_banner();
        if (race) {
          race_hud = race->hud_info(), d.race = &race_hud;
          race_hud.results_page = results_page;
          race_hud.won_car_name = won_car_name;
          if (!race->player_finished()) results_table = false;
          race_hud.results_table = results_table && results_page == 0;
          results_points = race_hud.wristband > 0;
          race_hud.balance = career.credits;
          if (grid_menu_open) {
            race_hud.phase = carview::RaceHud::kGrid;
            race_hud.grid_menu_open = true;
            char dbuf[64];
            std::snprintf(dbuf, sizeof dbuf, "< DIFFICULTY: %s (+%d%%) >", difficulty_name(race_options.difficulty),
                          race::difficulty_bonus_percent(int(race_options.difficulty)));
            race_hud.grid_menu = {"START RACE", dbuf, "QUIT RACE"};
            race_hud.grid_selected = std::clamp(grid_menu_selected, 0, int(race_hud.grid_menu.size()) - 1);
            int64_t cid = 0;
            int clid = 2;
            current_car_info(cid, clid);
            const std::string cname = !class_bands.empty() && size_t(clid) < class_bands.size()
                                          ? class_bands[size_t(clid)].name
                                          : "?";
            const int cpi = [&]() {
              if (const garage::Car* sc = career_garage.selected_car(); sc && !class_bands.empty())
                return garage::shown_index(sc->performance_index, class_bands);
              if (const auto* sp = have_career_catalog ? career_catalog.find_car(cid) : nullptr; sp && !class_bands.empty())
                return garage::shown_index(sp->stock_pi, class_bands);
              return 0;
            }();
            race_hud.grid_car_info = carview::car_name(car) + "  [" + cname + " " + std::to_string(cpi) + "]";
          }
          // Wristband progress after award: show COLLECT IT FROM RACE CENTRAL whenever a new wristband is pending!
          if (race_hud.phase == carview::RaceHud::kFinished && race_hud.wristband > 0 && !wristbands.levels.empty()) {
            const int after = career.wristband_points, before = after - race_hud.wristband;
            const int pending = have_career_catalog ? career_catalog.pending_wristband(after, collected_wristband) : -1;
            const int la = pending >= 0 ? pending : wristbands.level_index(after);
            const int cur_i = std::clamp(collected_wristband, 0, int(wristbands.levels.size()) - 1);
            race_hud.band_before = before, race_hud.band_after = after;
            race_hud.band_lo = wristbands.levels[size_t(cur_i)].first;
            race_hud.band_hi = size_t(cur_i + 1) < wristbands.levels.size() ? wristbands.levels[size_t(cur_i + 1)].first : -1;
            if (la >= 0 && size_t(la) < wristbands.levels.size())
              race_hud.band_name = carview::game_text(wristbands.levels[size_t(la)].second);
            if (race_hud.band_hi > 0) race_hud.band_next = carview::game_text(wristbands.levels[size_t(cur_i + 1)].second);
            race_hud.band_up = pending >= 0;
          }
        }
        if (!race_line) d.toast = banner;
        d.popup_title = popup.title, d.popup_text = popup.text, d.popup_at = popup.at;
        d.popup_progress = popup.progress, d.popup_colour = popup.colour;
        // Career header panels (pause menu & sub-menus).
        d.credits = career.credits;
        d.popularity_rank = current_pop_rank();
        if (!wristbands.levels.empty()) {
          const int cur_i = std::clamp(collected_wristband, 0, int(wristbands.levels.size()) - 1);
          const int pending = have_career_catalog ? career_catalog.pending_wristband(career.wristband_points, collected_wristband) : -1;
          d.band_points = career.wristband_points;
          d.band_lo = wristbands.levels[size_t(cur_i)].first;
          d.band_hi = size_t(cur_i + 1) < wristbands.levels.size() ? wristbands.levels[size_t(cur_i + 1)].first : -1;
          d.band_name = carview::game_text(wristbands.levels[size_t(cur_i)].second) + " WRISTBAND";
          d.band_level = cur_i + 1;
          d.band_collect_ready = pending >= 0;
          const int next_i = pending >= 0 ? pending : (cur_i + 1);
          if (next_i >= 0 && size_t(next_i) < wristbands.levels.size())
            d.band_next = carview::game_text(wristbands.levels[size_t(next_i)].second);
        }
        d.pause = st.menu_title == "paused";
        d.car_name = carview::car_name(car);
        if (const garage::Car* sc = career_garage.selected_car(); sc && !class_bands.empty()) {
          d.car_class = class_bands[size_t(garage::class_of(sc->performance_index, class_bands))].name;
          d.car_pi = garage::shown_index(sc->performance_index, class_bands);
        } else if (have_career_catalog && !class_bands.empty()) {
          if (const auto* sp = career_catalog.find_car_by_media(car)) {
            d.car_class = class_bands[size_t(garage::class_of(sp->stock_pi, class_bands))].name;
            d.car_pi = garage::shown_index(sp->stock_pi, class_bands);
          }
        }
        d.menu_title = st.menu_title;
        d.menu = st.menu;
        d.menu_selected = st.menu_selected;
        carview::CareerBoard board;
        if (career_page.is_open()) {
          board = career_board(career_page);
          if (road_discovery.ready()) {
            board.roads = road_discovery.discovered_count();
            board.roads_total = road_discovery.total_roads();
          }
          if (have_outposts) {
            board.outposts = outpost_progress.discovered_count(outposts);
            board.outposts_total = int(outposts.outposts.size());
            board.pr_stunts = outpost_progress.completed_stunt_count(outposts);
            board.pr_stunts_total = int(outposts.stunts.size());
          }
          if (have_skills_data) {
            for (const auto& sc : skills_data.sponsor_challenges) {
              carview::SponsorChallengeRow srow;
              srow.sponsor_name = sc.sponsor_name.empty() ? sc.id : sc.sponsor_name;
              srow.challenge_title = sc.challenge_name.empty() ? sc.id : sc.challenge_name;
              srow.logo_key = sc.sponsor_name.empty() ? sc.id : sc.sponsor_name;
              srow.max_tiers = std::max(1, int(sc.tiers.size()));
              if (skill_tracker) {
                if (auto it = skill_tracker->sponsor_progress().find(sc.id);
                    it != skill_tracker->sponsor_progress().end()) {
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
          d.career = &board;
        }
        carview::RallyStageHud rally_stage_hud;
        carview::RallyHubState rally_hub_state;
        carview::RallyServiceAreaState rally_service_state;
        if (demo_rally_service && !rally_session.active()) {
          if (!rally_catalog.loaded) {
            const auto db_ptr = parts_db.valid() ? parts_db.get() : std::shared_ptr<cars::PartsDb>{};
            if (!*car_names_loaded) carview::car_name("");
            std::string rerr;
            race::load_rally_catalog(state_root, db_ptr.get(), car_names->strings(), rally_catalog, &rerr);
          }
          if (rally_catalog.loaded && !rally_catalog.championships.empty()) {
            rally_session.start_championship(rally_catalog, rally_catalog.championships.front().event_id,
                                             int(race_options.difficulty));
            rally_session.record_stage_finish(rally_catalog, 134.82);
            rally_session.advance_from_service_area();
            rally_session.record_stage_finish(rally_catalog, 148.19);
            rally_service_open = true;
            rally_service_selected = 0;
            rally_service_repaired = false;
          }
        }
        if (demo_club && !club_open) {
          club_check();
          if (club_gate.empty() && club_car >= 0 && club_data.cars.count(club_car)) {
            const auto& clist = club_data.cars.at(club_car);
            drive::SavedClubCar* rec = club_record();
            if (!rec) {
              drive::SavedClubCar nrec;
              nrec.car_id = club_car;
              for (const auto& ch : clist) {
                nrec.challenges.emplace_back(ch.definition_hash, false);
                nrec.progress.emplace_back();
              }
              saved.club.push_back(std::move(nrec));
              rec = club_record();
            }
            if (rec && rec->challenges.size() >= 2) {
              rec->challenges[0].second = true;
              rec->challenges[1].second = true;
              club_check();
              club_ensure_meta();
              const fh1::StringTables* stbls = car_names->strings();
              const auto ms = club_medal_stats();
              club_ceremony = {};
              club_ceremony.active = true;
              club_ceremony.challenge_title = clist[1].format_title(stbls);
              club_ceremony.challenge_desc = clist[1].format_description(stbls, &club_event_names);
              club_ceremony.car_name = carview::car_name(car);
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
        if (race && race->is_stage() && (race_hud.phase == carview::RaceHud::kRacing || demo_rally_stage)) {
          rally_stage_hud.active = true;
          rally_stage_hud.championship_name = rally_session.active() ? rally_session.title() : "BEAUMONT RALLY";
          rally_stage_hud.stage_name = race_hud.event;
          rally_stage_hud.stage_number = rally_session.active() ? (rally_session.current_stage_index() + 1) : 1;
          rally_stage_hud.total_stages = rally_session.active() ? rally_session.total_stages() : 4;
          rally_stage_hud.stage_clock = race_hud.clock;
          rally_stage_hud.progress = race_hud.progress;
          rally_stage_hud.total_splits = std::max(4, int(race->stage_track().splits.size()));
          rally_stage_hud.splits_passed = int(race->split_times().size());
          rally_stage_hud.last_split_delta = race->last_split_delta();
          rally_stage_hud.last_split_age = race_hud.clock - race->last_split_at();
          rally_stage_hud.pace = race->codriver().hud(race_hud.clock);
          if (demo_rally_stage && rally_stage_hud.splits_passed == 0) {
            rally_stage_hud.splits_passed = 1;
            rally_stage_hud.last_split_delta = -1.42;
            rally_stage_hud.last_split_age = 0.8;
          }
          if (demo_rally_stage && !rally_stage_hud.pace.active) {
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
        if (rally_hub_open && rally_catalog.loaded) {
          int64_t cid = 0;
          int clid = 4;
          current_car_info(cid, clid);
          const int disc = free_roam ? freeroam::upgrade_discount(free_roam->signs_found()) : 0;
          saved.credits = career.credits;
          const auto db_ptr = parts_db.valid() ? parts_db.get() : std::shared_ptr<cars::PartsDb>{};
          rally_hub_state = carview::make_rally_hub_state(rally_catalog, saved, have_garage ? &career_garage : nullptr,
                                                          db_ptr.get(), class_bands, car, clid, d.car_pi, disc,
                                                          rally_hub_selected, rally_notice);
          d.rally_hub = &rally_hub_state;
        }
        if (rally_service_open && rally_session.active()) {
          const double dmg_pct =
              std::clamp(double(std::max({car_damage.scratch_amount, car_damage.engine_damage,
                                          car_damage.trans_damage, car_damage.brake_damage, car_damage.zone_front,
                                          car_damage.zone_rear, car_damage.zone_left, car_damage.zone_right})) *
                             100.0,
                         0.0, 100.0);
          rally_service_state = carview::make_rally_service_state(rally_session, d.car_name, d.car_class, d.car_pi,
                                                                  dmg_pct, rally_service_repaired,
                                                                  rally_service_selected, rally_notice);
          d.rally_service = &rally_service_state;
        }
        if (rally_results_open) d.rally_results = &rally_results_state;
        d.touch_hud_active = touch_controls.visible(vehicle.time());
        carview::ui::List list;
        // Minimap under HUD shapes; hidden in menus, title screen, ceremony, Rally/Club screens, and grid menu.
        if (mm_size > 0 && !show_title && !ceremony.active && !map_overlay.active && !grid_menu_open && st.menu.empty() &&
            !career_page.is_open() && !rally_hub_open && !rally_service_open && !rally_results_open && !club_open)
          list.image(mm_x, mm_y, mm_x + mm_size, mm_y + mm_size, mm_uv);
        carview::build_drive_hud(list, d, float(width), float(height));
        if (club_open) {
          const auto club_sc = build_club_screen();
          carview::draw_club_screen(list, width, height, club_sc);
        } else if (!show_title && !ceremony.active && !rally_hub_open && !rally_service_open && !rally_results_open &&
                   st.menu.empty()) {
          const auto cstrip = build_club_active_strip();
          if (cstrip.active) carview::draw_club_active_strip(list, width, height, cstrip);
        }
        if (club_ceremony.active) {
          club_ceremony.age = demo_club ? 1.2 : (vehicle.time() - club_ceremony_at);
          club_ceremony.dock_bottom_right = club_open;
          if (club_ceremony.age > club_ceremony.duration) club_ceremony.active = false;
          else carview::draw_club_medal_ceremony(list, width, height, club_ceremony);
        }
        if (!show_title && !ceremony.active && radio_popup_gpu.alpha > 0.005f)
          carview::draw_radio_popup_gpu(list, radio_popup_gpu, float(width), float(height));
        if (story_intro.active()) story_intro.draw_hud(list, float(width), float(height));
        if (fmv_player.active()) {
          list.clear();
          fmv_player.draw(list, float(width), float(height));
        }
        if (rewind_buf.active()) {
          // 921_I_REWIND overlay: KITPARTS/REWINDLOGO, KITPARTS/REWINDARROW,
          // MASKS/GRADIATEDBAR timeline + BUTTON_BAR_SCENE.
          const float wf = float(width), hf = float(height), sf = hf / 720.0f;
          (void)carview::ui_scene("921_I_REWIND");
          carview::ui4::subtle_vignette(list, wf, hf, 0.35f);
          const char* rw_title = "REWIND";
          std::vector<carview::ui4::Prompt> rw_prompts;
          if (rewind_buf.is_rewinding()) {
            rw_title = rewind_buf.ui_state() == drive::RewindUiState::kLoopRewind ? "REWINDING FURTHER" : "REWINDING";
            rw_prompts = {{"B", "CANCEL"}};
          } else if (rewind_buf.is_paused_at_keyframe()) {
            rw_title = "REWIND PAUSED";
            if (rewind_buf.at_oldest_slot()) rw_prompts = {{"A", "RESUME"}, {"B", "CANCEL"}};
            else rw_prompts = {{"A", "RESUME"}, {"Y", "REWIND FURTHER"}, {"B", "CANCEL"}};
          } else if (rewind_buf.is_settling()) {
            rw_title = "RESUMING";
          }
          const float cx = 0.5f * wf;
          const float logo_y = hf - 190.0f * sf;
          std::array<float, 4> rw_logo{}, rw_arrow{}, rw_bar{};
          if (carview::ui_texture("KITPARTS/REWINDLOGO", rw_logo)) {
            list.image(cx - 120.0f * sf, logo_y, cx + 120.0f * sf, logo_y + 44.0f * sf, rw_logo, carview::ui4::kWhite);
          }
          if (carview::ui_texture("KITPARTS/REWINDARROW", rw_arrow)) {
            list.image(cx - 168.0f * sf, logo_y + 6.0f * sf, cx - 126.0f * sf, logo_y + 38.0f * sf, rw_arrow,
                       rewind_buf.is_rewinding() ? carview::ui4::kPink : carview::ui4::kWhite);
          }
          const float pill_y = logo_y + 52.0f * sf;
          carview::ui4::brush(list, cx - 180.0f * sf, pill_y, cx + 180.0f * sf, pill_y + 32.0f * sf,
                              carview::ui4::kPink);
          list.text(cx, pill_y + 7.0f * sf, rw_title,
                    carview::ui4::style(carview::ui::Font::kE, 18.0f * sf, carview::ui4::kWhite,
                                        carview::ui::Align::kCenter));
          // Gradiated rewind bar beneath badge
          const float bar_y = pill_y + 40.0f * sf;
          if (carview::ui_texture("MASKS/GRADIATEDBAR", rw_bar)) {
            list.image(cx - 220.0f * sf, bar_y, cx + 220.0f * sf, bar_y + 10.0f * sf, rw_bar, carview::ui4::kPink);
          } else {
            list.rect(cx - 220.0f * sf, bar_y, cx + 220.0f * sf, bar_y + 8.0f * sf, carview::ui4::kBrushBlack);
            list.rect(cx - 220.0f * sf, bar_y, cx + 120.0f * sf, bar_y + 8.0f * sf, carview::ui4::kPink);
          }
          carview::ui4::button_bar(list, rw_prompts, wf, hf);
        }
        if (slice12.trainer_overlay.is_open()) {
          list.clear();
          carview::draw_trainer_ui4(list, float(width), float(height), slice12.trainer_overlay, slice12.trainer,
                                    career.credits, std::max(0, collected_wristband), slice12.save_tree_label);
        } else if (slice12.cam_editor_overlay.is_open()) {
          list.clear();
          carview::draw_camera_editor_ui4(list, float(width), float(height), slice12.cam_editor_overlay,
                                          slice12.ht_receiver.current_pose());
        } else if (mods_page.is_open()) {
          list.clear();
          carview::draw_mods_page(list, float(width), float(height), mods_page);
        } else if (save_page.is_open()) {
          list.clear();
          carview::draw_save_page(list, float(width), float(height), save_page, save_session);
        } else if (settings_page.is_open()) {
          list.clear();
          carview::draw_settings_ui4(list, float(width), float(height), settings_page, settings_live_overlay);
        } else if (events_page.is_open()) {
          list.clear();
          carview::draw_events_ui4(list, float(width), float(height), events_page, class_bands);
        } else if (outpost_hub_open) {
          const freeroam::Outpost* op = have_outposts ? outposts.find_outpost(outpost_hub_id) : nullptr;
          std::vector<carview::OutpostMissionCard> mcards;
          mcards.reserve(outpost_hub_items.size());
          for (const auto& item : outpost_hub_items) {
            carview::OutpostMissionCard mc;
            mc.title = item.detail_title.empty() ? item.label : item.detail_title;
            mc.detail_lines = item.detail_lines;
            if (item.kind == OutpostHubItem::kPrStunt) {
              mc.is_stunt = true;
              const freeroam::PrStunt* pst = have_outposts ? outposts.find_stunt(item.ref_id) : nullptr;
              mc.completed = outpost_progress.is_stunt_completed(item.ref_id);
              mc.kind_label = pst ? freeroam::pr_stunt_kind_name(pst->kind) : "PR STUNT";
              mc.art_style = (pst && pst->kind == freeroam::PrStuntKind::kPhoto)   ? 12
                             : (pst && pst->kind == freeroam::PrStuntKind::kSkill) ? 11
                                                                                   : 10;
              if (pst) {
                if (pst->kind == freeroam::PrStuntKind::kSpeed)
                  mc.target_line = "TARGET: " + std::to_string(int(std::lround(pst->target_mph))) + " MPH";
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
              sw_paint.body.rgb = sw->rgb;
              sw_paint.body.finish = sw->metallic ? cars::PaintFinish::kMetallic : cars::PaintFinish::kNormal;
              have_sw_paint = true;
            }
            if (!target_media.empty() && target_media != car) {
              auto it = festival_guests.find(target_media);
              if (it == festival_guests.end()) {
                auto gs = std::make_unique<Showroom>(renderer);
                gs->set_guest();
                std::string ge;
                if (gs->load(bake_dir, target_media, ShowroomOptions{}, ge) && gs->upload(ge)) {
                  gs->load_car_lamps(game_root_of(), target_media);
                  it = festival_guests.emplace(target_media, std::move(gs)).first;
                } else {
                  it = festival_guests.emplace(target_media, nullptr).first;
                }
              }
              if (it != festival_guests.end() && it->second) {
                if (have_sw_paint) it->second->set_paint_spec(sw_paint);
                showroom.set_draw_car(false);
                Showroom::Instance inst{};
                inst.car = it->second.get();
                inst.model[0] = 1.0f;
                inst.model[5] = 1.0f;
                inst.model[10] = 1.0f;
                inst.model[15] = 1.0f;
                showroom.set_instances({inst});
              }
            } else {
              showroom.set_draw_car(true);
              if (have_sw_paint) showroom.set_paint_spec(sw_paint);
            }
          }
          list.clear();
          carview::draw_autoshow_hud(list, float(width), float(height), autoshow, class_bands, career.credits,
                                     vehicle.time() < notice_until ? notice : std::string());
        } else if (shop.is_open()) {
          list.clear();
          carview::draw_upgrade_shop_hud(list, float(width), float(height), shop, class_bands, discount(),
                                         career.credits, carview::car_name(car),
                                         vehicle.time() < notice_until ? notice : std::string());
        } else if (paint_open && livery_editor.is_open()) {
          list.clear();
          carview::draw_livery_editor_hud(list, float(width), float(height), livery_editor, carview::car_name(car),
                                          vehicle.time() < notice_until ? notice : std::string());
        } else if (garage_open && have_garage && !career_garage.cars.empty()) {
          if (garage_browser.visible().empty()) {
            const cars::PartsDb* gdb =
                (parts_db.valid() && parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    ? parts_db.get().get()
                    : nullptr;
            garage_browser.refresh(career_garage, gdb, class_bands);
          }
          if (!garage_browser.visible().empty()) {
            list.clear();
            carview::draw_garage_hud(list, float(width), float(height), garage_browser, class_bands, career.credits,
                                     vehicle.time() < notice_until ? notice : std::string());
          }
        }
        if (barn_card.visible) {
          carview::draw_barn_find_hud(list, float(width), float(height), barn_card);
        }
        const bool show_track_chip = !show_title && !fmv_player.active() && !barn_card.visible && !ceremony.active && !map_overlay.active && !grid_menu_open &&
                                     st.menu.empty() && !career_page.is_open() && !settings_page.is_open() &&
                                     !shop.is_open() && !autoshow.is_open() && !garage_open &&
                                     !events_page.is_open() && !outpost_hub_open && !paint_open && !club_open;
        achievements.build_hud_overlays(list, float(width), float(height), show_track_chip);
        if (show_track_chip) {
          touch_controls.build_overlay(list, float(width), float(height), vehicle.time());
        }
        auto append_modern_overlays = [&](carview::ui::List& target_list) {
          if (perf_overlay_open) {
            const auto vram = renderer.vram_stats();
            const auto ws = showroom.world_draw_stats();
            const auto up = renderer.active_upscaler();
            const auto fg = renderer.active_frame_gen();
            carview::PerfInfo pinfo;
            const auto rex = renderer.render_width()
                                 ? carview::modern::Extent{renderer.render_width(), renderer.render_height()}
                                 : renderer.expected_render_extent(uint32_t(width), uint32_t(height));
            const uint32_t out_w = renderer.target_width() ? renderer.target_width() : uint32_t(width);
            const uint32_t out_h = renderer.target_height() ? renderer.target_height() : uint32_t(height);
            pinfo.fps = frame_stats.fps();
            pinfo.fps_1pct_low = frame_stats.ms_low_fps(0.01);
            pinfo.rss_mib = double(carview::bench::query_process_memory().rss_bytes) / (1024.0 * 1024.0);
            pinfo.cpu_ms = last_frame_cpu_ms >= 0.0 ? last_frame_cpu_ms : frame_stats.ms_avg();
            if (headless && pinfo.fps < 0.5 && pinfo.cpu_ms > 0.1) pinfo.fps = 1000.0 / pinfo.cpu_ms;
            pinfo.generated_fps = frame_stats.generated_fps();
            pinfo.render_w = rex.w;
            pinfo.render_h = rex.h;
            pinfo.output_w = out_w;
            pinfo.output_h = out_h;
            pinfo.gpu_ms = renderer.gpu_frame_ms();
            pinfo.gpu_stages = renderer.gpu_stages();
            pinfo.vram_used = vram.device_local_usage ? vram.device_local_usage : vram.allocation_bytes;
            pinfo.vram_budget = vram.device_local_budget;
            pinfo.resident_draws = showroom.gpu_resident_draws();
            pinfo.resident_cells = pinfo.resident_draws;
            pinfo.draws_drawn = uint32_t(ws.drawn);
            pinfo.draws_culled = uint32_t(ws.culled);
            pinfo.hdr = renderer.hdr_active() != carview::modern::HdrMode::kOff;
            pinfo.vsync = active_graphics.vsync == carview::modern::VSync::kImmediate ? "IMMEDIATE"
                          : active_graphics.vsync == carview::modern::VSync::kMailbox ? "MAILBOX"
                                                                                       : "FIFO";
            pinfo.upscaler = up.label;
            pinfo.frame_generation = carview::modern::frame_gen_name(fg);
            pinfo.device = renderer.device_name();
            pinfo.build = "shift-engine slice11";
            carview::build_perf_overlay(target_list, pinfo, float(width), float(height), 1.0f);
          }
          if (inspector_open) {
            renderer.set_hiz_preview_view(showroom.hiz_image_view());
            std::vector<uint8_t> insp_rgba;
            uint32_t insp_w = 0, insp_h = 0;
            if (renderer.take_inspector_preview(insp_rgba, insp_w, insp_h) && insp_w > 0 && insp_h > 0) {
              has_inspector_preview = carview::ui::atlas_update("inspector.preview", insp_w, insp_h, insp_rgba,
                                                                inspector_preview_uv);
            }
            const auto vram = renderer.vram_stats();
            const auto ws = showroom.world_draw_stats();
            const auto up = renderer.active_upscaler();
            const auto rex = renderer.render_width()
                                 ? carview::modern::Extent{renderer.render_width(), renderer.render_height()}
                                 : renderer.expected_render_extent(uint32_t(width), uint32_t(height));
            carview::FrameInspectorSnapshot snap;
            snap.open = true;
            snap.view = inspector_view;
            snap.render_w = rex.w;
            snap.render_h = rex.h;
            snap.output_w = renderer.target_width() ? renderer.target_width() : uint32_t(width);
            snap.output_h = renderer.target_height() ? renderer.target_height() : uint32_t(height);
            snap.cpu_ms = last_frame_cpu_ms >= 0.0 ? last_frame_cpu_ms : frame_stats.ms_avg();
            snap.gpu_ms = renderer.gpu_frame_ms();
            snap.pass_timings = renderer.gpu_stages();
            snap.world_draws = uint32_t(ws.drawn);
            snap.world_culled = uint32_t(ws.culled);
            snap.world_triangles = uint32_t(ws.indices / 3);
            snap.shadow_draws = uint32_t(showroom.shadow_draw_count());
            snap.shadow_cars = uint32_t(showroom.shadow_car_count());
            snap.resident_draws = showroom.gpu_resident_draws();
            snap.resident_cells = snap.resident_draws;
            snap.vram_used = vram.device_local_usage ? vram.device_local_usage : vram.allocation_bytes;
            snap.vram_budget = vram.device_local_budget;
            snap.vrs_active = renderer.vrs_active();
            snap.reverse_z = renderer.reverse_z();
            snap.z_prepass = z_prepass;
            snap.hdr_mode = renderer.hdr_active() == carview::modern::HdrMode::kHdr10
                                ? "HDR10"
                                : renderer.hdr_active() == carview::modern::HdrMode::kScRgb ? "scRGB" : "SDR";
            snap.upscaler = carview::modern::upscaler_name(up.active);
            snap.preview_uv = inspector_preview_uv;
            snap.has_preview = has_inspector_preview;
            carview::build_frame_inspector(target_list, snap, float(width), float(height));
          }
        };
        append_modern_overlays(list);
        upload_ui_atlas(renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        renderer.set_ui(list.items());
        if (!legacy_hud_cleared) renderer.set_overlay({}, 0, 0, 0, 0, 0), legacy_hud_cleared = true;
        hud_height = int(carview::drive_hud_dial_height(float(height)));
      };
      // GPU vector-font radio popup (Slice 9: replaces CPU 5x7 draw_radio_popup).
      auto set_radio_popup = [&](int /*height*/) {
        const auto tel = audio_host.telemetry();
        const audio::Radio::Popup p = audio_host.has_radio() && !tel.user_music_active ? tel.radio_popup : audio::Radio::Popup{};
        if (p.alpha <= 0 || p.station < 0 || size_t(p.station) >= audio_host.radio_data().stations.size()) {
          radio_popup_gpu = {};
          return;
        }
        const auto& st = audio_host.radio_data().stations[size_t(p.station)];
        radio_popup_gpu.station_name = st.name;
        radio_popup_gpu.station_label = st.off ? "RADIO OFF" : st.name;
        radio_popup_gpu.off = st.off;
        if (p.track >= 0 && size_t(p.track) < st.tracks.size()) {
          radio_popup_gpu.title = st.tracks[size_t(p.track)].title;
          radio_popup_gpu.artist = st.tracks[size_t(p.track)].artist;
        } else {
          radio_popup_gpu.title.clear();
          radio_popup_gpu.artist.clear();
        }
        radio_popup_gpu.alpha = float(std::clamp(p.alpha, 0.0, 1.0));
      };
      auto radio_press = [&](int dir) {
        if (!audio_host.has_radio()) return;
        audio_host.radio_press(dir);
      };
      // The maps (SPEC-FREEROAM-COLLECTIBLES): the minimap in the lower-left
      // corner (overlay layer 1, redrawn up to 15 times a second of drive
      // time) and the pause map (layer 2).
      auto map_scene = [&]() {
        MapScene sc;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
        sc.x = at.x;
        sc.z = at.z;
        sc.heading = std::atan2(f.x, f.z);
        sc.speed_mps = std::fabs(vehicle.speed());
        sc.data = progress_loaded ? &collectibles : nullptr;
        sc.progress = progress_loaded ? &progress : nullptr;
        sc.road_discovery = road_discovery.ready() ? &road_discovery : nullptr;
        sc.reveal = reveal || treasure_map_setting || slice12.trainer.effective_reveal_collectibles();
        if (traffic && traffic->cars()) {
          sc.traffic.reserve(traffic->cars()->size());
          for (const auto& tc : *traffic->cars()) sc.traffic.push_back({tc.position.x, tc.position.z});
        }
        if (race) {
          race->opponent_positions(sc.opponents), race->route_near(sc.route);
        } else {
          if (gps_nav.active()) {
            gps_nav.remaining_polyline(sc.gps_route);
            sc.has_gps_dest = true;
            sc.gps_dest_x = gps_nav.route().snapped_dest_x;
            sc.gps_dest_z = gps_nav.route().snapped_dest_z;
          }
          if (have_career_catalog) {
          // Populate Festival sites, unlocked Street Hubs, and unlocked Career Events!
          for (const auto& site : career_catalog.sites) {
            MapMarker mm;
            mm.kind = MapMarker::kFestivalSite;
            mm.icon_tag = site.map_tag;
            mm.id = site.name;
            mm.title = site.name;
            mm.subtitle = site.kind == race::FestivalSite::kRaceCentral &&
                                  career_catalog.pending_wristband(career.wristband_points, collected_wristband) >= 0
                              ? "NEW WRISTBAND READY TO COLLECT!"
                              : "HORIZON FESTIVAL";
            mm.x = site.pos[0], mm.z = site.pos[2];
            mm.is_new = site.kind == race::FestivalSite::kRaceCentral &&
                        career_catalog.pending_wristband(career.wristband_points, collected_wristband) >= 0;
            mm.site_or_hub = int(site.kind);
            sc.markers.push_back(std::move(mm));
          }
          for (const auto& hub : career_catalog.hubs) {
            if (!hub.has_trigger || !career_catalog.is_hub_unlocked(hub, career, collected_wristband)) continue;
            MapMarker mm;
            mm.kind = MapMarker::kStreetHub;
            mm.icon_tag = "streetrace";
            mm.id = "HUB_" + std::to_string(hub.id);
            mm.title = "STREET RACE HUB " + std::to_string(hub.id);
            mm.subtitle = "ILLEGAL STREET RACING";
            mm.x = hub.trigger_pos[0], mm.z = hub.trigger_pos[2];
            mm.site_or_hub = hub.id;
            sc.markers.push_back(std::move(mm));
          }
          const int rank = current_pop_rank();
          for (const auto& ev : career_catalog.events) {
            if (!ev.has_trigger || !career_catalog.is_event_unlocked(ev, career, collected_wristband, rank)) continue;
            MapMarker mm;
            mm.kind = MapMarker::kEvent;
            mm.icon_tag = race::CareerCatalog::event_map_tag(ev);
            mm.id = ev.horizon_id;
            const std::string ename = carview::game_text(ev.name_ref);
            mm.title = (!ename.empty() && ename.rfind("_&", 0) != 0) ? ename : ev.horizon_id;
            const int bp = career_catalog.best_place(ev, career);
            mm.completed = bp > 0;
            mm.is_new = !mm.completed;
            mm.subtitle = std::string(race::CareerCatalog::event_style_label(ev)) + "  •  " +
                          carview::thousands(int64_t(ev.cash_prize)) + " Cr";
            mm.x = ev.trigger_pos[0], mm.z = ev.trigger_pos[2];
            sc.markers.push_back(std::move(mm));
          }
          }
          if (have_outposts) {
            for (const auto& op : outposts.outposts) {
              MapMarker mm;
              mm.kind = MapMarker::kOutpost;
              mm.icon_tag = "gas_station";
              mm.id = op.id;
              mm.title = op.display_name;
              mm.discovered = outpost_progress.is_outpost_discovered(op.id);
              int done_st = 0;
              for (const auto& mid : op.mission_ids) done_st += outpost_progress.is_stunt_completed(mid);
              mm.completed = !op.mission_ids.empty() && done_st == int(op.mission_ids.size());
              mm.is_new = !mm.discovered;
              mm.subtitle = mm.discovered
                                ? ("HORIZON OUTPOST  •  PR STUNTS " + std::to_string(done_st) + "/" +
                                   std::to_string(op.mission_ids.size()) + "  •  FAST TRAVEL")
                                : "HORIZON OUTPOST  •  DRIVE HERE TO UNLOCK FAST TRAVEL";
              mm.x = op.trigger.x;
              mm.z = op.trigger.z;
              mm.site_or_hub = op.index;
              sc.markers.push_back(std::move(mm));
            }
            for (const auto& st : outposts.stunts) {
              MapMarker mm;
              mm.kind = MapMarker::kPrStunt;
              mm.icon_tag = st.kind == freeroam::PrStuntKind::kSpeed ? "speed_camera_discovered" : "exhibition";
              mm.id = st.id;
              mm.title = st.title;
              mm.completed = outpost_progress.is_stunt_completed(st.id);
              mm.is_new = !mm.completed;
              mm.subtitle = std::string(freeroam::pr_stunt_kind_name(st.kind)) + "  •  " +
                            carview::thousands(st.reward_credits) + " Cr";
              mm.x = st.target_pos.x;
              mm.z = st.target_pos.z;
              sc.markers.push_back(std::move(mm));
            }
          }
          if (speed_tracker) {
            for (const auto& t : speed_data.traps) {
              const double pb = speed_tracker->best_trap_mph(t.id);
              MapMarker mm;
              mm.kind = MapMarker::kSpeedTrap;
              mm.icon_tag = pb > 0.0 ? "speed_camera_completed" : "speed_camera_discovered";
              mm.id = t.id;
              mm.title = "SPEED TRAP " + t.id.substr(t.id.rfind('_') + 1);
              mm.completed = pb > 0.0;
              mm.is_new = !mm.completed;
              char sub[64];
              if (pb > 0.0) std::snprintf(sub, sizeof sub, "SPEED TRAP  •  PB %.1f MPH", pb);
              else std::snprintf(sub, sizeof sub, "SPEED TRAP  •  MIN %.0f MPH", t.min_speed_mph);
              mm.subtitle = sub;
              mm.x = t.gate.center[0];
              mm.z = t.gate.center[2];
              sc.markers.push_back(std::move(mm));
            }
            for (const auto& z : speed_data.zones) {
              const double pb = speed_tracker->best_zone_mph(z.id);
              MapMarker mm;
              mm.kind = MapMarker::kSpeedZone;
              mm.icon_tag = pb > 0.0 ? "average_speed_camera_completed" : "average_speed_camera_discovered";
              mm.id = z.id;
              mm.title = "SPEED ZONE " + z.id.substr(z.id.rfind('_') + 1);
              mm.completed = pb > 0.0;
              mm.is_new = !mm.completed;
              char sub[64];
              if (pb > 0.0) std::snprintf(sub, sizeof sub, "SPEED ZONE  •  PB %.1f MPH AVG", pb);
              else std::snprintf(sub, sizeof sub, "SPEED ZONE  •  AVG SPEED CHALLENGE");
              mm.subtitle = sub;
              mm.x = z.midpoint[0];
              mm.z = z.midpoint[2];
              sc.markers.push_back(std::move(mm));
            }
          }
          if (festival_mgr) {
            for (const auto& fc : festival_mgr->cars()) sc.festival_ai.push_back({fc.position.x, fc.position.z});
            if (festival_mgr->h2h_active()) {
              sc.route = festival_mgr->h2h_route();
              const auto fin = festival_mgr->h2h_finish_pos();
              MapMarker mm;
              mm.kind = MapMarker::kH2hFinish;
              mm.icon_tag = "race_encounter_finish";
              mm.id = "H2H_FINISH";
              mm.title = "HEAD-TO-HEAD FINISH";
              mm.subtitle = "FIRST TO THE FINISH WINS THE WAGER";
              mm.x = fin[0];
              mm.z = fin[2];
              sc.markers.push_back(std::move(mm));
            }
          }
          sc.selected_marker = map_selected_marker;
        }
        return sc;
      };
      double minimap_at = -1;
      auto set_minimap = [&](int height, bool force) {
        if (!map_view.ready() || !streamer) return;
        // The results card: no minimap.
        if (race && race->player_finished()) {
          if (minimap_at > -2) mm_size = 0, minimap_at = -2;
          return;
        }
        const double t = vehicle.time();
        if (!force && minimap_at >= 0 && t >= minimap_at && t - minimap_at < 1.0 / 15) return;
        minimap_at = t;
        // SPEC-UI-LAYOUTS a: 90,420-280,600 at 1280x720, no frame.
        // When Android touch controls are visible, lift into non-overlapping safe rect.
        const double k = height / 720.0;
        const bool touch_vis = touch_controls.visible(vehicle.time());
        carview::TouchRect mm_safe, tach_safe;
        int req_px = std::max(96, int(std::lround(180 * k)));
        if (touch_vis) {
          carview::TouchControls::hud_safe_rects(float(width), float(height), mm_safe, tach_safe);
          req_px = std::max(96, int(std::lround(std::min(mm_safe.w() * float(width), mm_safe.h() * float(height)))));
        }
        HudImage img = map_view.minimap(map_scene(), req_px);
        if (carview::ui::atlas_update("minimap", img.width, img.height, img.rgba, mm_uv)) {
          mm_size = float(img.width);
          if (touch_vis) {
            mm_x = mm_safe.x0 * float(width);
            mm_y = mm_safe.y1 * float(height) - mm_size;
          } else {
            mm_x = float(90 * k), mm_y = float(height) - float(120 * k) - mm_size;
          }
        } else {
          mm_size = 0;
        }
      };
      if (status == 0 && streamer) {
        const auto& st = streamer->stats();
        std::printf("scenery: %zu triangles, %zu materials, %zu textures decoded (%zu failed), %zu models with the "
                    "default UV scale, %zu submeshes without a material\n",
                    streamer->mesh().triangles(), streamer->mesh().materials.size(), texture_loader.decoded,
                    texture_loader.failed.size(), st.default_uv_models.size(), st.no_material);
        if (st.far_models)
          std::printf("far ring: %zu models, %zu triangles\n", st.far_models, st.far_triangles);
        if (st.trees || st.trees_untextured)
          std::printf("tree sprites: %zu (%zu without an atlas file)\n", st.trees, st.trees_untextured);
        std::printf("props: %zu placed, %zu unresolved; left out: %zu off the free-roam route, %zu markers, %zu beacons, %zu "
                    "tree sprites off the route\n",
                    st.props, st.prop_unresolved, st.props_off_route, st.props_markers, st.props_beacons,
                    st.trees_off_route);
        for (const auto& n : showroom.notes()) std::fprintf(stderr, "carview: note: %s\n", n.c_str());
        // The effects drawn without a texture, by triangles.
        const auto& mesh = streamer->mesh();
        std::map<std::string, size_t> flat;
        for (size_t t = 0; t < mesh.material.size(); ++t) {
          const auto& m = mesh.materials[mesh.material[t]];
          if (m.albedo == world::kNoTexture || texture_loader.failed.count(m.albedo)) ++flat[m.effect];
        }
        for (const auto& [effect, n] : flat)
          std::printf("  drawn flat (slot 0 has no texture file): %s, %zu triangles\n", effect.c_str(), n);
        if (std::getenv("SHIFT_WORLD_DEBUG")) {
          // Per material: triangles, median UV span per metre of edge.
          std::map<uint32_t, std::vector<float>> spans;
          for (size_t t = 0; t < mesh.material.size(); ++t) {
            const auto& a = mesh.positions[t * 3];
            const auto& b = mesh.positions[t * 3 + 1];
            const double len = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            const auto& ua = mesh.uv0[t * 3];
            const auto& ub = mesh.uv0[t * 3 + 1];
            if (len > 0.5) spans[mesh.material[t]].push_back(float(std::hypot(ub[0] - ua[0], ub[1] - ua[1]) / len));
          }
          for (auto& [m, v] : spans) {
            std::sort(v.begin(), v.end());
            const auto& mm = mesh.materials[m];
            double sum[4] = {};
            size_t corners = 0;
            for (size_t t = 0; t < mesh.material.size(); ++t)
              if (mesh.material[t] == m)
                for (int k = 0; k < 3; ++k, ++corners)
                  for (int c = 0; c < 4; ++c) sum[c] += mesh.colours[t * 3 + size_t(k)][size_t(c)];
            if (corners)
              std::printf("  material %u vertex colour mean %.0f %.0f %.0f %.0f\n", m, sum[0] / corners,
                          sum[1] / corners, sum[2] / corners, sum[3] / corners);
            auto id = [&](uint32_t r) { return r < colorado.textures().size() ? colorado.textures()[r].id : 0u; };
            std::printf("  material %u %s texture %u (_0x%08X) layers %d B _0x%08X splat _0x%08X tiling %.3g %.3g "
                        "%.3g %.3g far %d blend %d cut-out %d: %zu triangles, UV per metre %.4g\n",
                        m, mm.effect.c_str(), mm.albedo, id(mm.albedo), int(mm.layers), id(mm.layer_b), id(mm.splat),
                        mm.tiling[0], mm.tiling[1], mm.tiling[2], mm.tiling[3], int(mm.far), int(mm.blend),
                        int(texture_loader.cutout.count(mm.albedo)), v.size(), v[v.size() / 2]);
          }
        }
      }
      if (status != 0) {
      } else if (!world_reference_path.empty()) {
        // The reference frame: file-frame positions (the scenery's are in
        // the game frame, relative to its origin), the reference's matrix
        // flipped to Vulkan's clip y, no car.
        const float ox = float(showroom.world_origin_x()), oz = float(showroom.world_origin_z());
        const float to_file[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, ox, 0, -oz, 1};
        showroom.set_world_transform(to_file);
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(to_file, kSunGame, sun);
        rotate_direction(to_file, up_game, up);
        showroom.set_world_light(sun, up, 1.0f);
        if (have_shadow_light) {
          float towards[3];
          rotate_direction(to_file, shadow_light_game, towards);
          showroom.set_shadow_direction(towards);
        }
        ShowroomFrame frame{};
        std::memcpy(frame.view_projection, world_reference.clip_from_world, sizeof frame.view_projection);
        for (int c = 0; c < 4; ++c) frame.view_projection[c * 4 + 1] = -frame.view_projection[c * 4 + 1];
        // Reversed depth: z' = w - z (the game's depth d becomes 1 - d).
        if (renderer.reverse_z())
          for (int c = 0; c < 4; ++c) frame.view_projection[c * 4 + 2] = frame.view_projection[c * 4 + 3] - frame.view_projection[c * 4 + 2];
        std::memcpy(frame.transparent_view_projection, frame.view_projection, sizeof frame.view_projection);
        for (int k = 0; k < 3; ++k) frame.eye[k] = world_reference.camera_position[k];
        frame.options[0] = 1;
        showroom.set_draw_car(false);
        showroom.set_frame(frame);
        // SHIFT_WORLD_PICK=x,y (debug, ours): the nearest scenery triangles
        // under that pixel of the reference view, with their materials.
        if (const char* pick = std::getenv("SHIFT_WORLD_PICK"); pick && streamer) {
          float px = 0, py = 0;
          std::sscanf(pick, "%f,%f", &px, &py);
          const auto& mesh = streamer->mesh();
          const float* m = world_reference.clip_from_world;
          std::vector<std::pair<float, size_t>> hits;
          for (size_t t = 0; t * 3 + 2 < mesh.positions.size(); ++t) {
            float sx[3], sy[3], sz[3];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
              const auto& v = mesh.positions[t * 3 + size_t(k)];
              const float f[4] = {float(v[0]), float(v[1]), -float(v[2]), 1};
              float c[4] = {};
              for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j) c[r] += m[j * 4 + r] * f[j];
              if (c[3] <= 0.01f) ok = false;
              sx[k] = (c[0] / c[3] * 0.5f + 0.5f) * float(width);
              sy[k] = (0.5f - c[1] / c[3] * 0.5f) * float(height);
              sz[k] = c[2] / c[3];
            }
            if (!ok || int(mesh.materials[mesh.material[t]].layers) == 4) continue;
            if ((sz[0] + sz[1] + sz[2]) / 3 < 0 || (sz[0] + sz[1] + sz[2]) / 3 > 1) continue;
            auto edge = [&](int a, int b) { return (sx[b] - sx[a]) * (py - sy[a]) - (sy[b] - sy[a]) * (px - sx[a]); };
            const float e0 = edge(0, 1), e1 = edge(1, 2), e2 = edge(2, 0);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
              hits.push_back({(sz[0] + sz[1] + sz[2]) / 3, t});
          }
          std::sort(hits.begin(), hits.end());
          auto id = [&](uint32_t r) { return r < colorado.textures().size() ? colorado.textures()[r].id : 0u; };
          for (size_t i = 0; i < hits.size() && i < 6; ++i) {
            const size_t t = hits[i].second;
            const auto& mm = mesh.materials[mesh.material[t]];
            if (fh1::WorldTexture wt; i == 0 && colorado.read_texture(mm.albedo, wt, nullptr) && !wt.blocks.levels.empty())
              std::printf("pick: albedo format %d %ux%u levels %zu gamma %d cut-out %d\n", int(wt.blocks.format),
                          wt.blocks.levels[0].width, wt.blocks.levels[0].height, wt.blocks.levels.size(),
                          int(wt.format.gamma), int(fh1::world_texture_has_cutout(wt)));
            std::printf("pick: depth %.5f tri %zu material %u %s albedo _0x%08X layers %d B _0x%08X splat _0x%08X "
                        "far %d blend %d surface %d colour %d %d %d %d model %s\n",
                        hits[i].first, t, mesh.material[t], mm.effect.c_str(), id(mm.albedo), int(mm.layers),
                        id(mm.layer_b), id(mm.splat), int(mm.far), int(mm.blend), int(mesh.surface[t]),
                        mesh.colours[t * 3][0], mesh.colours[t * 3][1], mesh.colours[t * 3][2], mesh.colours[t * 3][3],
                        t < mesh.model.size() && mesh.model[t] < colorado.models().size()
                            ? colorado.models()[mesh.model[t]].name.c_str() : "?");
          }
        }
        std::vector<uint8_t> rgba;
        if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          const std::filesystem::path path(out);
          if (!write_png(path.string(), width, height, 4, rgba.data())) status = 1;
          // Side by side with the reference image.
          int rw = 0, rh = 0, rc = 0;
          unsigned char* ref = world_reference.image.empty()
                                   ? nullptr
                                   : stbi_load(world_reference.image.string().c_str(), &rw, &rh, &rc, 4);
          if (ref && uint32_t(rw) == width && uint32_t(rh) == height) {
            std::vector<uint8_t> both(size_t(width) * 2 * height * 4);
            for (uint32_t y = 0; y < height; ++y) {
              std::memcpy(&both[(size_t(y) * width * 2) * 4], ref + size_t(y) * width * 4, size_t(width) * 4);
              std::memcpy(&both[(size_t(y) * width * 2 + width) * 4], &rgba[size_t(y) * width * 4], size_t(width) * 4);
            }
            const auto vs = path.parent_path() / (path.stem().string() + "_vs.png");
            if (!write_png(vs.string(), width * 2, height, 4, both.data())) status = 1;
            std::printf("world reference: %s and %s\n", path.string().c_str(), vs.string().c_str());
          } else {
            std::printf("world reference: %s (no reference image of the same size)\n", path.string().c_str());
          }
          if (ref) stbi_image_free(ref);
        }
      } else if (bench) {
        // Comprehensive cross-device benchmark suite (JSON schema v1 + per-frame CSV).
        // Backward-compatible with legacy --bench, and supports deterministic
        // --bench-scenario modes, --bench-full-fx, --bench-audio, and --bench-warmup.
        using S = carview::bench::Scenario;
        const bool audio_bench_on = sound && (bench_audio || bench_full_fx) && audio_host.has_audio();
        if (audio_bench_on) audio_host.start_benchmark_device();

        // Scenario-specific initialization.
        PauseMenu bench_pause_menu;
        bool autoshow_opened = false;
        if (bench_scenario == S::kAutoshowStudio) {
          if (parts_db.valid()) {
            if (const auto db = parts_db.get()) {
              std::string why;
              autoshow_opened = autoshow.open(*db, all_baked, why, 100000000, false, false, 1.0);
              if (autoshow_opened) autoshow.set_step(carview::AutoshowStep::kCarSelect);
            }
          }
          // Studio lighting: use staticcarcubemap.xpr (loaded before upload) rather than outdoor sky.
          showroom.set_draw_car(true);
        }

        const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
        carview::bench::BenchmarkRecorder recorder;
        recorder.reserve(size_t(steps / 2 + 16));

        double step_total_accum = 0.0;
        step_physics_ms = 0.0;
        step_ai_traffic_ms = 0.0;
        step_world_stream_ms = 0.0;

        bool ttff_recorded = false;
        bool measured_started = false;
        double measured_cpu0 = process_cpu_seconds();
        auto measured_wall0 = std::chrono::steady_clock::now();
        const auto run_wall0 = measured_wall0;
        const uint64_t staging_bytes0 = renderer.staging_upload_bytes() + showroom.world_staging_bytes();
        uint32_t frame_idx = 0;

        // Legacy summary accumulators (for backward-compatible stdout lines).
        std::vector<double> legacy_cpu_ms, legacy_gpu_ms, legacy_sim_ms, legacy_record_ms, legacy_pose_ms;
        double draws_sum = 0, culled_sum = 0, indices_sum = 0, shadow_sum = 0, shadow_cars_sum = 0;
        std::vector<std::string> stage_names;
        std::vector<std::vector<double>> stage_ms;

        for (int s = 0; s <= steps && status == 0; ++s) {
          const auto t0 = std::chrono::steady_clock::now();
          if (s > 0 && bench_scenario != S::kAutoshowStudio) {
            if (!step(cruise ? follower.input(vehicle) : drive::script_input(script, vehicle.time()))) {
              std::fprintf(stderr, "carview: %s\n", error.c_str());
              status = 1;
              break;
            }
          }
          step_total_accum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          if (s % 2 != 0) continue;

          const auto frame_t0 = std::chrono::steady_clock::now();
          const double sim_sec = double(frame_idx) * 2.0 * drive::Vehicle::kStep;
          const bool is_warmup = (bench_warmup_sec > 0.0) && (sim_sec < bench_warmup_sec);
          if (!is_warmup && !measured_started) {
            measured_started = true;
            measured_cpu0 = process_cpu_seconds();
            measured_wall0 = frame_t0;
          }

          // Audio push phase.
          double audio_push_ms = 0.0;
          if (audio_bench_on) {
            const auto a0 = std::chrono::steady_clock::now();
            const audio::View aview = audio_view();
            audio_host.update_frame(sound_inputs(aview), false, autoshow.is_open() || events_page.is_open() || settings_page.is_open(), false, false);
            audio::VehicleAudioState vst{};
            const drive::Vec3 vpos = vehicle.position();
            const drive::Vec3 vvel = vehicle.velocity();
            const drive::Vec3 vfwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
            const drive::Vec3 vright = drive::rotate(vehicle.orientation(), {1, 0, 0});
            vst.speed_mph = float(std::fabs(vehicle.speed()) * 2.2369363);
            vst.rpm = float(vehicle.rpm());
            vst.redline_rpm = float(std::max(vehicle.params().redline_rpm, 6000.0));
            vst.throttle = float(vehicle.engine_throttle());
            vst.gear = vehicle.gear();
            vst.listener_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
            vst.listener_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
            vst.listener_right = {float(vright.x), float(vright.y), float(vright.z)};
            vst.cockpit_view = (aview == audio::View::kCockpit);
            vst.in_race = (race != nullptr);
            audio_host.update_vehicle(vst, race != nullptr, false, false, 0);
            audio_host.update_world_acoustics(float(vpos.x), float(vpos.y), float(vpos.z),
                                              float(std::atan2(vfwd.x, vfwd.z)));
            audio_push_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a0).count();
          }

          // Pose / camera / TOD / cull prep phase.
          const auto p0 = std::chrono::steady_clock::now();
          if (bench_scenario == S::kAutoshowStudio) {
            if (autoshow_opened && frame_idx > 0 && frame_idx % 15 == 0) {
              autoshow.move(1);
              autoshow.move_colour(1);
            }
            Orbit o;
            o.yaw = 145.0f + float(frame_idx) * 1.2f;
            o.pitch = 10.0f;
            o.distance = 0.0f;
            showroom.set_frame(orbit_frame(showroom, o, float(width) / float(height), false));
          } else {
            wheel_poses(vehicle, poses);
            if (photo_mode.active()) {
              const float wb = photo_mode.effects().wheel_blur_omega_boost();
              for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wb;
            }
            showroom.set_wheel_poses(poses);
            showroom.set_braking(brake_glow(vehicle));
            tod_cockpit_cam = (bench_scenario == S::kCockpitMirrors) ||
                              (have_rig && rig.mode() == drive::CameraMode::kCockpit);
            place_world();
            showroom.set_frame(drive_frame(float(width) / float(height), 2 * drive::Vehicle::kStep));
          }
          const double pose_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();

          // UI4 build phase (including ui4_heavy multi-screen cycling & autoshow carousel).
          double ui_build_ms = 0.0;
          const bool need_hud = hud || bench_scenario == S::kUi4Heavy || bench_scenario == S::kAutoshowStudio;
          if (need_hud) {
            const auto u0 = std::chrono::steady_clock::now();
            const PauseMenu* active_menu = nullptr;
            if (bench_scenario == S::kUi4Heavy) {
              const uint32_t ui_phase = (frame_idx / 10u) % 4u;
              map_overlay.active = false;
              events_page.close();
              settings_page.close();
              if (ui_phase == 0) {
                map_overlay.active = true;
                map_overlay.title = "COLORADO HORIZON FESTIVAL";
                map_overlay.subtitle = "RED ROCK CANYON  •  FESTIVAL CENTRAL";
                map_overlay.action_hint = "ENTER EVENT";
                map_overlay.gps_hint = "SET GPS ROUTE";
                map_overlay.filter_tabs = {"ALL", "EVENTS", "OUTPOSTS", "SIGNS", "SPEED TRAPS"};
                map_overlay.active_filter = int(frame_idx % 5u);
                map_overlay.zoom = 1.0 + 0.25 * double(frame_idx % 6u);
                map_overlay.roads_discovered = 142;
                map_overlay.roads_total = 216;
              } else if (ui_phase == 1) {
                bench_pause_menu.show();
                bench_pause_menu.move(1);
                active_menu = &bench_pause_menu;
              } else if (ui_phase == 2) {
                if (have_career_catalog) {
                  int64_t cid = 0;
                  int clid = 2;
                  current_car_info(cid, clid);
                  events_page.open(career_catalog, career, collected_wristband, current_pop_rank(), cid, clid,
                                   class_bands, int((frame_idx / 40u) % 3u));
                  for (uint32_t mv = 0; mv < (frame_idx % 8u); ++mv) events_page.move(1);
                } else {
                  bench_pause_menu.show();
                  active_menu = &bench_pause_menu;
                }
              } else {
                settings_page.open();
                for (uint32_t mv = 0; mv < (frame_idx % 5u); ++mv) settings_page.switch_tab(1);
                for (uint32_t mv = 0; mv < (frame_idx % 6u); ++mv) settings_page.move(1);
              }
            }
            set_hud(int(width), int(height), race ? race_banner() : intro, active_menu);
            ui_build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();
          }

          double rec_ms = 0.0, gpu_ms = -1.0;
          renderer.set_camera_projection(g_cam_near, g_cam_far, g_cam_proj00, g_cam_proj11);
          renderer.set_camera_vp(g_cam_vp_unjittered, 0.0f, 0.0f);
          if (!renderer.render_timed(FrameConstants{}, width, height, rec_ms, gpu_ms, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          const double submit_wait_ms = renderer.last_submit_wait_ms();
          const auto frame_t1 = std::chrono::steady_clock::now();
          if (!ttff_recorded) {
            ttff_recorded = true;
            bench_startup.ttff_ms =
                std::chrono::duration<double, std::milli>(frame_t1 - drive_start_tp).count();
          }

          const double active_cpu_ms = step_physics_ms + step_ai_traffic_ms + step_world_stream_ms +
                                       audio_push_ms + pose_ms + ui_build_ms + rec_ms;
          const double total_frame_ms =
              step_total_accum + std::chrono::duration<double, std::milli>(frame_t1 - frame_t0).count();

          carview::bench::FrameSample fs{};
          fs.frame_index = frame_idx;
          fs.is_warmup = is_warmup;
          fs.sim_sec = sim_sec;
          fs.wall_sec = std::chrono::duration<double>(frame_t1 - run_wall0).count();
          fs.frame_ms = total_frame_ms;
          fs.cpu_ms = active_cpu_ms;
          fs.gpu_ms = gpu_ms;
          fs.physics_ms = step_physics_ms;
          fs.ai_traffic_ms = step_ai_traffic_ms;
          fs.world_stream_ms = step_world_stream_ms;
          fs.audio_push_ms = audio_push_ms;
          fs.ui_build_ms = ui_build_ms;
          fs.pose_cull_ms = pose_ms;
          fs.cmd_record_ms = rec_ms;
          fs.submit_wait_ms = submit_wait_ms;
          fs.gpu_passes = renderer.stage_ms();

          const auto ws = showroom.world_draw_stats();
          fs.world_draws = uint32_t(ws.drawn);
          fs.world_culled = uint32_t(ws.culled);
          fs.world_hiz_culled = showroom.gpu_hiz_culled();
          fs.trees_drawn = showroom.gpu_trees_drawn();
          fs.shadow_world_draws = uint32_t(showroom.shadow_draw_count());
          fs.shadow_cars = uint32_t(showroom.shadow_car_count());
          fs.env_draws = showroom.last_env_draws();
          fs.mirror_draws = showroom.last_mirror_draws();
          fs.car_draws = showroom.last_car_draws();
          fs.car_instances = uint32_t(showroom.instance_count());
          fs.particle_sprites = showroom.last_particle_sprites();
          fs.skid_tris = showroom.last_skid_tris();
          fs.ui4_shapes = renderer.last_ui_shape_count();
          fs.triangles_drawn = uint64_t(ws.indices / 3u) + uint64_t(showroom.triangle_count());
          fs.triangles_total = uint64_t(showroom.world_triangle_count()) + uint64_t(showroom.triangle_count());
          fs.rss_mib = double(carview::bench::query_process_memory().rss_bytes) / (1024.0 * 1024.0);
          recorder.add_frame(fs);

          if (!is_warmup) {
            legacy_sim_ms.push_back(step_total_accum);
            legacy_pose_ms.push_back(pose_ms + ui_build_ms);
            legacy_record_ms.push_back(rec_ms);
            legacy_cpu_ms.push_back(step_total_accum + pose_ms + ui_build_ms + rec_ms);
            legacy_gpu_ms.push_back(gpu_ms);
            draws_sum += double(ws.drawn);
            culled_sum += double(ws.culled);
            indices_sum += double(ws.indices);
            shadow_sum += double(showroom.shadow_draw_count());
            shadow_cars_sum += double(showroom.shadow_car_count());
            for (const auto& [name, ms] : renderer.stage_ms()) {
              size_t k = 0;
              while (k < stage_names.size() && stage_names[k] != name) ++k;
              if (k == stage_names.size()) {
                stage_names.push_back(name);
                stage_ms.emplace_back();
              }
              stage_ms[k].push_back(ms);
            }
          }

          step_total_accum = 0.0;
          step_physics_ms = 0.0;
          step_ai_traffic_ms = 0.0;
          step_world_stream_ms = 0.0;
          ++frame_idx;
        }

        // Clean up temporary UI4/Autoshow state before finalizing.
        if (bench_scenario == S::kUi4Heavy) {
          map_overlay.active = false;
          events_page.close();
          settings_page.close();
        }
        if (autoshow_opened) {
          autoshow.close();
          showroom.set_instances({});
          showroom.set_draw_car(true);
        }

        const double measured_wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - measured_wall0).count();
        const double measured_cpu = process_cpu_seconds() - measured_cpu0;

        // cold_boot_switch: 5 deterministic car hot-switches measuring load + GPU upload + first frame.
        if (status == 0 && bench_scenario == S::kColdBootSwitch) {
          std::vector<std::string> switch_targets;
          for (const auto& c : baked_cars) {
            if (lower_ascii(c) != lower_ascii(car)) switch_targets.push_back(c);
            if (switch_targets.size() >= 5) break;
          }
          if (switch_targets.empty() && !car.empty()) switch_targets.push_back(car);
          for (const auto& sw_car : switch_targets) {
            carview::bench::CarSwitchSample sw{};
            sw.car = sw_car;
            auto gs = std::make_unique<Showroom>(renderer);
            gs->set_guest();
            std::string ge;
            const auto sw_t0 = std::chrono::steady_clock::now();
            const bool loaded = gs->load(bake_dir, sw_car, ShowroomOptions{}, ge);
            const auto sw_t1 = std::chrono::steady_clock::now();
            const bool uploaded = loaded && gs->upload(ge);
            const auto sw_t2 = std::chrono::steady_clock::now();
            if (uploaded) {
              gs->load_car_lamps(game_root_of(), sw_car);
              showroom.set_draw_car(false);
              Showroom::Instance inst{};
              inst.car = gs.get();
              inst.model[0] = inst.model[5] = inst.model[10] = inst.model[15] = 1.0f;
              showroom.set_instances({inst});
              double srec = 0.0, sgpu = -1.0;
              renderer.render_timed(FrameConstants{}, width, height, srec, sgpu, ge);
              showroom.set_instances({});
              showroom.set_draw_car(true);
              renderer.wait_idle();
            }
            const auto sw_t3 = std::chrono::steady_clock::now();
            sw.load_ms = std::chrono::duration<double, std::milli>(sw_t1 - sw_t0).count();
            sw.upload_ms = std::chrono::duration<double, std::milli>(sw_t2 - sw_t1).count();
            sw.total_ms = std::chrono::duration<double, std::milli>(sw_t3 - sw_t0).count();
            bench_startup.car_switches.push_back(std::move(sw));
          }
        }

        // Gather final audio telemetry before stopping the benchmark audio thread.
        carview::bench::AudioMetrics audio_metrics{};
        audio_metrics.enabled = audio_bench_on;
        if (audio_bench_on) {
          const auto atel = audio_host.telemetry();
          audio_metrics.blocks_rendered = atel.blocks_rendered;
          audio_metrics.avg_block_ms = atel.avg_block_ms;
          audio_metrics.p95_block_ms = atel.p95_block_ms;
          audio_metrics.worst_block_ms = atel.worst_block_ms;
          audio_metrics.stalls = atel.stalls;
          audio_host.stop_device();
        }

        // Assemble hardware, GPU, features, build, memory, streaming, and UI4 atlas reports.
        const auto sys_hw = carview::bench::probe_system_hardware();
        const auto& rdev = renderer.device_info();
        const auto vram = renderer.vram_stats();
        const double vram_total_mib = double(renderer.device_local_bytes()) / (1024.0 * 1024.0);
        const double vram_budget_mib = double(vram.device_local_budget) / (1024.0 * 1024.0);

        carview::bench::GpuInfo gpu_info{};
        gpu_info.device_name = rdev.device_name.empty() ? renderer.device_name() : rdev.device_name;
        gpu_info.vendor_id = rdev.vendor_id;
        gpu_info.device_id = rdev.device_id;
        gpu_info.device_type = rdev.device_type;
        gpu_info.api_version_raw = rdev.api_version;
        gpu_info.api_version = carview::bench::format_vulkan_version(rdev.api_version);
        gpu_info.driver_version_raw = rdev.driver_version;
        gpu_info.driver_name = rdev.driver_name;
        gpu_info.driver_info = rdev.driver_info;
        gpu_info.is_moltenvk = rdev.is_moltenvk;
        gpu_info.is_turnip = rdev.is_turnip;
        gpu_info.portability_subset = rdev.portability_subset;
        gpu_info.has_timestamps = rdev.has_timestamps;
        gpu_info.vram_total_mib = vram_total_mib;
        gpu_info.vram_budget_mib = vram_budget_mib;

        carview::bench::FeatureFlags feat{};
        feat.gpu_driven = renderer.gpu_driven_supported();
        feat.hiz = renderer.gpu_driven_supported() && showroom.hiz_image_view() != VK_NULL_HANDLE;
        feat.bindless = renderer.gpu_driven_supported();
        feat.vrs_supported = renderer.vrs_supported();
        feat.vrs_active = renderer.vrs_active();
        feat.hdr10_supported = renderer.hdr10_supported();
        feat.scrgb_supported = renderer.scrgb_supported();
        feat.hdr_mode = renderer.hdr_active() == carview::modern::HdrMode::kHdr10   ? "hdr10"
                        : renderer.hdr_active() == carview::modern::HdrMode::kScRgb ? "scrgb"
                                                                                    : "off";
        feat.upscaler = carview::modern::upscaler_name(renderer.active_upscaler().active);
        feat.frame_gen = carview::modern::frame_gen_name(renderer.active_frame_gen());
        feat.aa_mode = active_graphics.aa == carview::modern::AaMode::kFxaaAll     ? "fxaa_all"
                       : active_graphics.aa == carview::modern::AaMode::kFxaaWorld ? "fxaa_world"
                                                                                   : "msaa";
        feat.ssao = active_graphics.ssao == carview::modern::SsaoQuality::kHigh     ? "high"
                    : active_graphics.ssao == carview::modern::SsaoQuality::kMedium ? "medium"
                    : active_graphics.ssao == carview::modern::SsaoQuality::kLow    ? "low"
                                                                                    : "off";
        feat.shadows = !shadows_flag.empty() ? shadows_flag : (low_preset ? "low" : "high");
        feat.vsync = active_graphics.vsync == carview::modern::VSync::kImmediate ? "immediate"
                     : active_graphics.vsync == carview::modern::VSync::kMailbox ? "mailbox"
                                                                                 : "fifo";
        feat.msaa = renderer.sample_count();
        feat.anisotropy = renderer.sampler_anisotropy();
        feat.render_scale = active_graphics.scale;
        feat.base_540p = active_graphics.base_540p;
        feat.z_prepass = z_prepass;
        feat.reverse_z = reverse_z;
        feat.bloom = bloom_flag >= 0 ? (bloom_flag == 1) : !low_preset;
        feat.full_fx = bench_full_fx;
        feat.bench_audio = audio_bench_on;

        carview::bench::BuildInfo binfo{};
#ifdef SHIFT_GIT_COMMIT
        binfo.git_commit = SHIFT_GIT_COMMIT;
#else
        binfo.git_commit = "unknown";
#endif
#ifdef SHIFT_BUILD_CONFIG
        binfo.build_config = SHIFT_BUILD_CONFIG;
#else
        binfo.build_config = "Release";
#endif
        binfo.preset = preset_name;
        binfo.device_label = device_name.empty() ? gpu_info.device_name : device_name;
        binfo.scenario = std::string(carview::bench::scenario_name(bench_scenario));
        binfo.output_width = width;
        binfo.output_height = height;
        binfo.render_width = renderer.render_width();
        binfo.render_height = renderer.render_height();
        binfo.target_seconds = seconds;
        binfo.warmup_seconds = bench_warmup_sec;

        bench_startup.pipeline_compile_ms = renderer.pipeline_compile_ms() + showroom.pipeline_compile_ms();

        const auto pmem = carview::bench::query_process_memory();
        carview::bench::MemoryMetrics mem_m{};
        mem_m.rss_current_mib = double(pmem.rss_bytes) / (1024.0 * 1024.0);
        mem_m.rss_peak_mib = double(pmem.peak_rss_bytes) / (1024.0 * 1024.0);
        mem_m.vma_blocks = vram.block_count;
        mem_m.vma_allocs = vram.allocation_count;
        mem_m.vma_used_mib = double(vram.allocation_bytes) / (1024.0 * 1024.0);
        mem_m.vma_reserved_mib = double(vram.block_bytes) / (1024.0 * 1024.0);
        mem_m.vram_usage_mib = double(vram.device_local_usage) / (1024.0 * 1024.0);
        mem_m.vram_budget_mib = vram_budget_mib;
        mem_m.vram_total_mib = vram_total_mib;

        const auto wts = showroom.world_texture_stats();
        const uint64_t staging_bytes1 = renderer.staging_upload_bytes() + showroom.world_staging_bytes();
        carview::bench::StreamingMetrics st_m{};
        st_m.resident_textures = wts.resident_count;
        st_m.resident_textures_mib = double(wts.resident_bytes) / (1024.0 * 1024.0);
        st_m.texture_uploads = wts.uploads;
        st_m.texture_evictions = wts.evictions;
        st_m.texture_reuploads = wts.reuploads;
        st_m.retired_pending = wts.retired_pending;
        st_m.descriptor_pools = wts.descriptor_pools;
        st_m.staging_upload_bytes = staging_bytes1 >= staging_bytes0 ? (staging_bytes1 - staging_bytes0) : 0;

        const auto astats = carview::ui::atlas_stats();
        carview::bench::Ui4AtlasMetrics ui_m{};
        ui_m.atlas_size = astats.atlas_size;
        ui_m.glyph_rows_used = astats.glyph_rows_used;
        ui_m.persistent_rows_used = astats.persistent_rows_used;
        ui_m.transient_rows_used = astats.transient_rows_used;
        ui_m.utilization_pct = astats.utilization_pct;
        ui_m.transient_evictions = astats.transient_evictions;
        ui_m.glyph_count = astats.glyph_count;
        ui_m.image_count = astats.image_count;

        const auto report = recorder.finalize(sys_hw, gpu_info, feat, binfo, bench_startup, audio_metrics,
                                              ui_m, mem_m, st_m, measured_wall, measured_cpu);

        // Emit comprehensive summary + legacy-compatible lines.
        std::printf("%s", carview::bench::format_console_summary(report).c_str());

        auto pct = [](std::vector<double> v, double q) {
          if (v.empty()) return 0.0;
          std::sort(v.begin(), v.end());
          return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
        };
        const size_t n = legacy_cpu_ms.size();
        if (n) {
          std::printf("bench: world draws per frame %.0f drawn, %.0f culled, %.0fk of %.0fk triangles drawn\n",
                      draws_sum / double(n), culled_sum / double(n), indices_sum / double(n) / 3000.0,
                      double(showroom.world_triangle_count()) / 1000.0);
          if (shadow_sum > 0)
            std::printf("bench: shadow casters per frame %.0f world draws, %.2f traffic cars\n",
                        shadow_sum / double(n), shadow_cars_sum / double(n));
        }

        if (!bench_json_path.empty()) {
          std::string jerr;
          if (!carview::bench::write_json(bench_json_path, report, jerr)) {
            std::fprintf(stderr, "carview: %s\n", jerr.c_str());
            status = 1;
          } else {
            std::printf("bench: wrote JSON -> %s\n", bench_json_path.string().c_str());
          }
        }
        if (!bench_csv_path.empty()) {
          std::string cerr;
          if (!carview::bench::write_csv(bench_csv_path, recorder.frames(), cerr)) {
            std::fprintf(stderr, "carview: %s\n", cerr.c_str());
            status = 1;
          } else {
            std::printf("bench: wrote CSV  -> %s (%zu frames)\n", bench_csv_path.string().c_str(),
                        recorder.frames().size());
          }
        }
        (void)pct;
      } else if (!demo_photo_png.empty()) {
        // Headless Slice 10 verification capture: Photo Mode UI + DoF/vignette + Horizon Promo.
        const drive::Vec3 pat = vehicle.position();
        const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
        const double phead = std::atan2(pf.x, pf.z);
        photo_mode.enter({pat.x, pat.y, pat.z}, phead,
                         carview::PhotoLimits::from_settings(live_photo_through_walls, true));
        photo_mode.effects().apply_preset(carview::PhotoPreset::kHorizonSunset);
        photo_mode.effects().aperture = 68.0f;
        photo_mode.effects().vignette = 42.0f;
        photo_mode.effects().shutter_speed = 45.0f;
        photo_mode.autofocus({pat.x, pat.y + 0.65, pat.z});
        wheel_poses(vehicle, poses);
        const float wboost = photo_mode.effects().wheel_blur_omega_boost();
        for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wboost;
        showroom.set_wheel_poses(poses);
        showroom.set_braking(0.35f);
        showroom.set_draw_car(true);
        tod_cockpit_cam = false;
        place_world();
        const float aspect = float(width) / float(height);
        const ShowroomFrame pframe =
            camera_view_frame(photo_mode.camera().view(), info, vehicle, aspect, renderer.reverse_z());
        showroom.set_frame(pframe);
        float cfw[16];
        compute_clip_from_world(pframe, info, vehicle, cfw);
        const auto promo_res = promo_tracker.evaluate(cfw, photo_mode.camera().eye(), gather_promo_candidates(), true);
        photo_mode.notify_photo_saved(demo_photo_png, promo_res);
        photo_mode.step(0.4, {});  // let shutter flash settle while keeping the Horizon Promo banner visible
        carview::ui::List plist;
        photo_mode.build_hud(plist, float(width), float(height));
        upload_ui_atlas(renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        renderer.set_ui(plist.items());
        std::vector<uint8_t> rgba;
        if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(demo_photo_png.string(), width, height, 4, rgba.data())) status = 1;
          else std::printf("demo photo mode -> %s (spotted %d/%d, +%lld Cr)\n", demo_photo_png.string().c_str(),
                           promo_res.total_spotted, promo_res.total_cars, (long long)promo_res.credits_awarded);
        }
        photo_mode.exit();
      } else if (!demo_replay_png.empty()) {
        // Headless Slice 10 verification capture: Replay Viewer HUD with timeline scrubber, telemetry & track-side camera.
        drive::ReplayMetadata rm;
        rm.event_name = "RED ROCK CANYON SPRINT";
        rm.world_name = "Colorado";
        rm.tod_sec = tod_clock.tod_sec;
        rm.sample_hz = 60.0;
        rm.is_race = true;
        rm.player_place = 1;
        rm.field_size = 8;
        rm.roster.push_back({0u, int32_t(info.car_id), car, "PLAYER - " + carview::car_name(car), true, false});
        replay_rec.begin(rm, 600, false);
        replay_rec.add_marker(0.0, "START");
        // Drive forward for 4.5 seconds on Colorado road to capture genuine physics motion + speed.
        const int demo_steps = int(std::lround(4.5 / drive::Vehicle::kStep));
        for (int ds = 0; ds <= demo_steps; ++ds) {
          const drive::Input di = ds == 0 ? drive::Input{} : (cruise ? follower.input(vehicle) : drive::Input{0.0, 0.92, 0.0, false});
          if (ds > 0 && !step(di)) break;
          std::vector<drive::ReplayCarSample> frame_cars;
          frame_cars.push_back(drive::ReplayCarSample::from_vehicle(0u, vehicle, player_lamps, di.throttle, di.brake,
                                                                    di.handbrake ? 1.0 : 0.0, di.steer, 1));
          replay_rec.maybe_record(vehicle.time(), std::move(frame_cars));
          if (ds == demo_steps / 2) replay_rec.add_marker(vehicle.time(), "SECTOR 1");
        }
        replay_rec.add_marker(vehicle.time(), "FINISH");
        replay_viewer.open(replay_rec.snapshot(), true);
        replay_viewer.set_camera_mode(carview::ReplayCameraMode::kTrackSide);
        replay_viewer.set_telemetry(true);
        replay_viewer.seek(replay_viewer.duration() * 0.62);
        replay_viewer.set_speed(0.5);
        // Apply sampled pose & wheel spin for rendering.
        if (const auto* fc = replay_viewer.focused_car()) {
          drive::Vehicle::Pose rp = vehicle.pose();
          fc->apply_to_pose(rp);
          vehicle.set_pose(rp);
          wheel_poses(vehicle, poses);
          for (int wi = 0; wi < 4; ++wi) poses[wi].omega = fc->wheel_omega[wi];
          showroom.set_wheel_poses(poses);
          showroom.set_braking((fc->lamp_bits & drive::kReplayLampBrake) ? 1.0f : 0.0f);
        }
        showroom.set_draw_car(true);
        tod_cockpit_cam = false;
        place_world();
        auto gb = [&](double x, double y, double z, double& gy) {
          drive::Hit hit;
          if (!ground || !ground->raycast({x, y, z}, {0, -1, 0}, 80.0, hit)) return false;
          gy = y - hit.distance;
          return true;
        };
        const drive::CameraView rv_cam = replay_viewer.compute_camera(0.016, 0.0f, 0.0f, 0.0f, gb);
        const float aspect = float(width) / float(height);
        showroom.set_frame(camera_view_frame(rv_cam, info, vehicle, aspect, renderer.reverse_z()));
        carview::ui::List rlist;
        replay_viewer.build_hud(rlist, float(width), float(height));
        upload_ui_atlas(renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        renderer.set_ui(rlist.items());
        std::vector<uint8_t> rgba;
        if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(demo_replay_png.string(), width, height, 4, rgba.data())) status = 1;
          else std::printf("demo replay view -> %s (%.1fs, %zu frames)\n", demo_replay_png.string().c_str(),
                           replay_viewer.duration(), replay_viewer.recording().frame_count());
        }
        replay_viewer.close();
      } else if (!demo_achievements_png.empty()) {
        // Headless Slice 10 verification capture: F7 Achievements screen with wristband header, tabs, progress bars & HUD unlock toast.
        place_world();
        const float aspect = float(width) / float(height);
        showroom.set_frame(drive_frame(aspect, 0.016));
        carview::AchievementGameplayState ds{};
        ds.career_started = true;
        ds.wristbands_earned = 3;
        ds.num_races = 22;
        ds.num_victories = 14;
        ds.num_podiums = 18;
        ds.street_races_won = 6;
        ds.festival_races_won = 8;
        ds.discount_signs_found = 78;
        ds.barns_found = 6;
        ds.roads_discovered = 184;
        ds.outposts_discovered = 6;
        ds.pr_stunts_completed = 9;
        ds.garage_cars = 12;
        ds.promo_cars_spotted = 34;
        ds.total_distance_m = 420000.0;
        ds.top_speed_mps = 102.0;
        ds.total_winnings = 1650000;
        ds.max_skills_score = 32500;
        ds.popularity_points = 185000;
        ds.driver_level = 25;
        ds.has_rally = true;
        ds.rally_stages_done = 5;
        ds.rally_victories = 2;
        ds.rally_level = 3;
        ds.club_medals_total = 14;
        ds.club_cars_with_medal = 5;
        achievements.evaluate(ds, false);
        achievements.set_tracked_id(194);
        achievements.open_screen();
        achievements.cycle_sort();  // NEAREST sort so tracked/in-progress achievement is focused in detail panel!
        achievements.unlock(179, 0, true);  // live toast showcase
        for (int ti = 0; ti < 12; ++ti) achievements.update(0.1);  // finish count-up & toast slide-in
        carview::ui::List alist;
        achievements.build_screen_hud(alist, float(width), float(height));
        achievements.build_hud_overlays(alist, float(width), float(height), false);
        upload_ui_atlas(renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        renderer.set_ui(alist.items());
        std::vector<uint8_t> rgba;
        if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(demo_achievements_png.string(), width, height, 4, rgba.data())) status = 1;
          else std::printf("demo achievements -> %s (%d/%d unlocked, %u/%uG, spa=%zu)\n",
                           demo_achievements_png.string().c_str(), achievements.unlocked_count(),
                           achievements.total_count(), achievements.earned_gamerscore(),
                           achievements.available_gamerscore(), spa_db.achievements.size());
        }
        achievements.close_screen();
      } else if (!demo_touch_hud_png.empty()) {
        // Headless Slice 13B verification capture: Android Touch Controls HUD overlay on live Colorado drive.
        const int demo_steps = int(std::lround(2.5 / drive::Vehicle::kStep));
        for (int ds = 0; ds <= demo_steps; ++ds) {
          const drive::Input di = ds == 0 ? drive::Input{} : (cruise ? follower.input(vehicle) : drive::Input{-0.12, 0.88, 0.0, false});
          if (ds > 0 && !step(di)) break;
        }
        wheel_poses(vehicle, poses);
        showroom.set_wheel_poses(poses);
        showroom.set_braking(0.0f);
        showroom.set_draw_car(true);
        tod_cockpit_cam = false;
        place_world();
        const float aspect = float(width) / float(height);
        showroom.set_frame(drive_frame(aspect, 0.016));
        touch_controls.set_visibility(carview::TouchVisibility::kAlwaysOn);
        touch_controls.set_steer_mode(carview::TouchSteerMode::kButtons);
        const carview::TouchLayout tlay = carview::TouchControls::compute_layout(float(width), float(height));
        touch_controls.on_finger_down(1, tlay.steer_left.cx(), tlay.steer_left.cy(), float(width), float(height), vehicle.time());
        touch_controls.on_finger_down(2, tlay.throttle.cx(), tlay.throttle.cy(), float(width), float(height), vehicle.time());
        set_minimap(int(height), true);
        set_hud(int(width), int(height), "ANDROID TOUCH CONTROLS - SMOOTH 60 PRESET");
        std::vector<uint8_t> rgba;
        if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(demo_touch_hud_png.string(), width, height, 4, rgba.data())) status = 1;
          else std::printf("demo touch hud -> %s (%ux%u)\n", demo_touch_hud_png.string().c_str(), width, height);
        }
      } else if (headless) {
        const std::filesystem::path prefix(out);
        const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
        const int stride = std::max(1, int(std::lround(every / drive::Vehicle::kStep)));
        double longest_step = 0;  // wall time of one step with its streaming, s
        double race_done_at = -1;
        for (int s = 0; s <= steps && status == 0; ++s) {
          const auto t0 = std::chrono::steady_clock::now();
          const bool ok =
              s == 0 || step(cruise ? follower.input(vehicle) : drive::script_input(script, vehicle.time()));
          if (audio_host.has_radio() && radio_press_every > 0 && s > 0) {
            const double t = vehicle.time(), was = t - drive::Vehicle::kStep;
            if (std::floor(t / radio_press_every) > std::floor(was / radio_press_every)) radio_press(+1);
          }
          if (audio_host.has_audio() && s > 0) {
            const int frames = int(std::lround(kAudioRate * drive::Vehicle::kStep));
            const size_t at = audio_track.size();
            audio_track.resize(at + 2 * size_t(frames), 0.0f);
            audio_host.render_offline(sound_inputs(audio_view()), audio_track.data() + at, frames);
          }
          longest_step =
              std::max(longest_step, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
          if (!ok) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          // A race ends the run 2 s after its last car finishes.
          if (race && race->session().all_finished()) {
            if (race_done_at < 0) race_done_at = vehicle.time();
            else if (vehicle.time() > race_done_at + 2) break;
          }
          if (s % stride != 0) continue;
          wheel_poses(vehicle, poses);
          if (photo_mode.active()) {
            const float wb = photo_mode.effects().wheel_blur_omega_boost();
            for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wb;
          }
          showroom.set_wheel_poses(poses);
          showroom.set_braking(brake_glow(vehicle));
          tod_cockpit_cam = have_rig && rig.mode() == drive::CameraMode::kCockpit;
          place_world();
          ShowroomFrame hframe = drive_frame(float(width) / float(height), every);
          float h_jitter_uv_x = 0.0f, h_jitter_uv_y = 0.0f;
          if (renderer.active_upscaler().temporal) {
            const auto rex = renderer.render_width()
                                 ? carview::modern::Extent{renderer.render_width(), renderer.render_height()}
                                 : renderer.expected_render_extent(width, height);
            const uint32_t jphase = carview::modern::jitter_phase_count(rex, {width, height});
            const auto j = carview::modern::halton_jitter(temporal_frame_idx++, jphase);
            apply_ndc_jitter(hframe, 2.0f * j[0] / float(std::max(1u, rex.w)),
                             2.0f * j[1] / float(std::max(1u, rex.h)));
            h_jitter_uv_x = j[0] / float(std::max(1u, rex.w));
            h_jitter_uv_y = j[1] / float(std::max(1u, rex.h));
          }
          showroom.set_frame(hframe);
          name_tags.clear();
          if (race && !race->player_finished()) {
            carview::RaceView::OpponentTag stag;
            if (race->showcase_tag(stag)) {
              float sx = 0, sy = 0;
              if (project_world_point(hframe, info, vehicle, stag.x, stag.y, stag.z, float(width), float(height), sx, sy))
                name_tags.push_back({sx, sy, stag.name, stag.leader});
            }
          } else if (festival_mgr) {
            const drive::Vec3 at = vehicle.position();
            for (const auto& fc : festival_mgr->cars()) {
              if (std::hypot(fc.position.x - at.x, fc.position.z - at.z) > 130.0) continue;
              float sx = 0, sy = 0;
              if (project_world_point(hframe, info, vehicle, fc.position.x, fc.position.y + 1.85, fc.position.z,
                                      float(width), float(height), sx, sy))
                name_tags.push_back({sx, sy, "[" + fc.spec.car_class + " " + std::to_string(fc.spec.pi) + "] " + fc.spec.driver_name,
                                     fc.in_h2h});
            }
          }
          if (hud || perf_overlay_open || inspector_open) {
            if (hud) set_minimap(int(height), true);
            set_hud(int(width), int(height), vehicle.time() < notice_until  ? notice
                                 : race ? race_banner()
                                 : vehicle.time() < kIntroSeconds ? intro
                                                                  : club_banner());
            if (hud) set_radio_popup(int(height));
          }
          std::vector<uint8_t> rgba;
          renderer.set_camera_projection(g_cam_near, g_cam_far, g_cam_proj00, g_cam_proj11);
          renderer.set_camera_vp(g_cam_vp_unjittered, h_jitter_uv_x, h_jitter_uv_y);
          const auto rt0 = std::chrono::steady_clock::now();
          if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          const auto rt1 = std::chrono::steady_clock::now();
          last_frame_cpu_ms = std::chrono::duration<double, std::milli>(rt1 - rt0).count();
          frame_stats.frame(std::chrono::duration<double>(rt1.time_since_epoch()).count());
          if (renderer.active_frame_gen() != carview::modern::FrameGen::kOff) frame_stats.generated(1);
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          char name[32];
          std::snprintf(name, sizeof name, "_%05d.png", int(std::lround(vehicle.time() * 100)));
          const std::string path = (prefix.parent_path() / (prefix.stem().string() + name)).string();
          if (!write_png(path, width, height, 4, rgba.data())) {
            std::fprintf(stderr, "carview: cannot write %s\n", path.c_str());
            status = 1;
          }
          if (texture_census && streamer) census_frame(width, height);
          const auto& wl = vehicle.wheels()[0];
          const drive::Vec3 at = vehicle.position();
          std::printf("t %.2f s: %.1f km/h, gear %d, %.0f rpm, LF steer %.3f rad compression %.3f m, at %.1f %.1f "
                      "%.1f -> %s\n",
                      vehicle.time(), vehicle.speed() * 3.6, vehicle.gear(), vehicle.rpm(), wl.steer, wl.compression,
                      at.x, at.y, at.z, path.c_str());
        }
        if (rig_max_distance > 0)
          std::printf("chase eye from the CoM over the run: %.2f to %.2f m\n", rig_min_distance, rig_max_distance);
        if (streamer)
          std::printf("longest step with its streaming: %.1f ms (%s gathers)\n", longest_step * 1000,
                      async_stream ? "background" : "synchronous");
        if (texture_census || streamer) {
          const auto vram = renderer.vram_stats();
          const auto wts = showroom.world_texture_stats();
          std::printf("vram summary: %u blocks, %u allocs (%.1f/%.1f MiB); world textures: resident %zu (%.1f MiB), uploads %llu, evictions %llu, reuploads %llu, pools %zu, retired_pending %zu\n",
                      vram.block_count, vram.allocation_count,
                      double(vram.allocation_bytes) / (1024.0 * 1024.0), double(vram.block_bytes) / (1024.0 * 1024.0),
                      wts.resident_count, double(wts.resident_bytes) / (1024.0 * 1024.0),
                      (unsigned long long)wts.uploads, (unsigned long long)wts.evictions,
                      (unsigned long long)wts.reuploads, wts.descriptor_pools, wts.retired_pending);
        }
        if (race) {
          std::printf("%s m\nrace: results at %.2f s%s\n%s", race->summary().c_str(), race->session().clock(),
                      race->session().all_finished() ? "" : " (not every car finished)", race->results().c_str());
        }
        if (audio_host.has_radio()) {
          const auto tel = audio_host.telemetry();
          if (tel.radio_selected >= 0 && size_t(tel.radio_selected) < audio_host.radio_data().stations.size()) {
            const auto& st = audio_host.radio_data().stations[size_t(tel.radio_selected)];
            std::printf("radio: %s at the end, %.1f s on the radio clock\n", st.off ? "off" : st.name.c_str(),
                        tel.radio_clock);
          }
        }
        if (!audio_out.empty() && !audio_track.empty()) {
          float peak = 0;
          for (float x : audio_track) peak = std::max(peak, std::fabs(x));
          if (!write_wav(audio_out, kAudioRate, audio_track))
            std::fprintf(stderr, "carview: cannot write %s\n", audio_out.string().c_str());
          else
            std::printf("audio: %.2f s to %s, peak %.3f\n", double(audio_track.size()) / 2 / kAudioRate,
                        audio_out.string().c_str(), peak);
        }
      } else {
        std::vector<SDL_Gamepad*> pads;
        {
          int pad_count = 0;
          if (SDL_JoystickID* pids = SDL_GetGamepads(&pad_count)) {
            for (int pi = 0; pi < pad_count; ++pi) {
              if (SDL_Gamepad* pad = SDL_OpenGamepad(pids[pi])) pads.push_back(pad);
            }
            SDL_free(pids);
          }
        }
        slice12.set_window(window);
        slice12.sync_pad_capabilities(pads);
        slice12.sync_wheel_devices();
        uint64_t last = SDL_GetTicksNS(), title_at = 0;
        double accumulator = 0;
        bool was_sim_paused = false;
        drive::Vehicle::Pose drawn_before = vehicle.pose();  // the pose before the last step
        bool running = true;
        std::printf("controls: W/Up throttle, S/Down brake (held at a stop: reverse), A/D or Left/Right steer, "
                    "Space handbrake, R put back on the road, V chase/front view, C (held) look back, H hide/show "
                    "the HUD, F12 save the pose (carview-pose-N.txt), Esc pause menu.\n"
                    "gamepad: right trigger throttle, left trigger brake, left stick steer, A handbrake, right stick "
                    "look around, Y (held) look back, Start pause menu, Back view.\n"
                    "car picker: [ and ] (LB and RB) choose among the %zu baked cars, Enter (X) drives it.\n",
                    baked_cars.size());
        // The picker's choice, shown in the title until it is taken.
        size_t pick = 0;
        bool picking = false;
        auto sync_pick = [&]() {
          const size_t synced = carview::sync_picker_index(baked_cars, pick_ids, career_garage.selected, lower_ascii(car));
          pick = synced;
          garage_pick = synced;
          if (baked_cars.empty()) picking = false;
        };
        sync_pick();
        sync_picker = sync_pick;
        auto browse = [&](int by) {
          if (baked_cars.empty()) return;
          if (pick >= baked_cars.size()) sync_pick();
          pick = (pick + baked_cars.size() + size_t(by + int(baked_cars.size()))) % baked_cars.size();
          picking = true;
          title_at = 0;  // show it now
        };
        auto take_pick = [&]() {
          if (!picking || baked_cars.empty()) return;
          if (pick >= baked_cars.size()) sync_pick();
          if (pick >= baked_cars.size()) return;
          // Restart with that car where this one stands.
          const drive::Vec3 at = vehicle.position();
          const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
          spawn[0] = at.x;
          spawn[1] = at.y;
          spawn[2] = at.z;
          spawn[3] = std::atan2(fwd.x, fwd.z);
          have_spawn = true;
          next_car = baked_cars[pick];
          if (pick < pick_ids.size()) {
            career_garage.selected = pick_ids[pick];
            write_career_garage();
          }
          running = false;
        };
        bool show_hud = true;
        PauseMenu menu;
        double next_save = 30;  // s of drive time: the save is also written every 30 s
        // Dedicated audio render thread (replaces main-thread audio rendering).
        audio_host.start_device();
        // R or Start: back on the nearest road (with a world), else to the start.
        auto put_back = [&]() {
          world::Spawn where;
          double away = 0;
          if (streamer && safe_reset_ok && safe_reset.place(*streamer, vehicle, &away))
            std::printf("t %.2f s: put back on the road %.1f m away\n", vehicle.time(), away);
          else if (streamer && world::recover(*streamer, vehicle, &where))
            std::printf("t %.2f s: put back on the road %.1f m away (nearest road triangle)\n", vehicle.time(),
                        where.distance);
          else
            vehicle.reset(start, start_heading);
          camera_pull = CameraPull{};
          rig.restart();
          rig_accel.reset();
          rewind_buf.reset_history(vehicle.time());
        };
        // Helpers for career progression actions (wristband collection, events, shops, new career).
        auto open_garage_browser = [&]() {
          sync_pick();
          garage_sell_confirm_id = -1;
          if (have_garage && !career_garage.cars.empty()) {
            const cars::PartsDb* gdb =
                (parts_db.valid() && parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    ? parts_db.get().get()
                    : nullptr;
            garage_browser.refresh(career_garage, gdb, class_bands);
            const auto& vis = garage_browser.visible();
            for (size_t gi = 0; gi < vis.size(); ++gi) {
              if (vis[gi].is_current) {
                const int cur_i = int(garage_browser.selected_index());
                if (int(gi) != cur_i) garage_browser.move(int(gi) - cur_i);
                break;
              }
            }
          }
          garage_open = true;
          audio_host.trigger_ui(audio::UiCue::kMenuOpen);
        };
        auto sync_livery_preview = [&](bool recomposite_vinyls) {
          showroom.set_paint_spec(livery_editor.document().paint);
          if (recomposite_vinyls) {
            std::vector<uint8_t> rgba;
            if (livery_editor.composite_1024(showroom.livery_planes().empty() ? nullptr : &showroom.livery_planes(),
                                             rgba, 1024)) {
              std::string lerr;
              showroom.set_custom_livery(rgba.data(), 1024, 1024, lerr);
            }
          }
        };
        auto open_paint = [&]() {
          paint_before = showroom.colour_index();
          const cars::PartsDb* db_ptr =
              (parts_db.valid() && parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                  ? parts_db.get().get()
                  : nullptr;
          int64_t cid = info.car_id;
          std::filesystem::path liv_p;
          if (have_garage) {
            if (const garage::Car* sel = career_garage.selected_car()) {
              cid = sel->car_id;
              if (!sel->livery_file.empty()) {
                liv_p = sel->livery_file;
                if (liv_p.is_relative() && !save_path.empty()) liv_p = save_path.parent_path() / liv_p;
              } else if (!save_path.empty()) {
                liv_p = save_path.parent_path() / "liveries" / ("car_" + std::to_string(sel->id) + ".liv");
              }
            }
          }
          if (liv_p.empty()) {
            const auto base_dir = !save_path.empty() ? save_path.parent_path() : carview::default_user_dir();
            liv_p = (base_dir.empty() ? std::filesystem::path("liveries") : (base_dir / "liveries")) / (car + ".liv");
          }
          livery_edit_path = liv_p;
          std::string msg;
          if (livery_editor.open(db_ptr, game_root_of(), cid, liv_p, msg)) {
            paint_open = true;
            notice = msg;
            notice_until = vehicle.time() + 3.0;
            audio_host.trigger_ui(audio::UiCue::kMenuOpen);
            return;
          }
          const auto& rows = showroom.colours();
          paint_order.resize(rows.size());
          for (size_t i = 0; i < rows.size(); ++i) paint_order[i] = i;
          std::stable_sort(paint_order.begin(), paint_order.end(),
                           [&](size_t a, size_t b) { return rows[a].sequence < rows[b].sequence; });
          paint_pick = 0;
          for (size_t i = 0; i < paint_order.size(); ++i)
            if (paint_order[i] == paint_before) paint_pick = i;
          paint_open = !paint_order.empty();
          if (!paint_open) notice = "this car has no colour rows", notice_until = vehicle.time() + 3;
        };
        auto open_shop_or_autoshow = [&](bool is_autoshow) {
          // Ensure a career garage exists so Autoshow & Upgrades always work smoothly.
          if (!have_garage && have_career_catalog) {
            if (const auto* spec = career_catalog.find_car_by_media(car))
              award_garage_car(spec->car_id, 0, true), garage_picker();
          }
          std::string why = "parts database unavailable";
          bool ready = false;
          if (have_garage && parts_db.valid()) {
            why = "loading parts database, try again in a second";
            if (parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
              why = "no parts database";
              if (const auto db = parts_db.get()) {
                const bool tutorial_first = !imported && career_garage.cars.size() <= 1 && career.wristband_points == 0;
                const double price_mult = slice12.trainer.pacing.effective_price_multiplier();
                ready = is_autoshow
                            ? autoshow.open(*db, all_baked, why, career.credits, tutorial_first,
                                            slice12.trainer.hidden_cars, price_mult)
                            : shop.open(*db, career_garage,
                                        career_garage.selected_car() ? career_garage.selected_car()->id : 0, why,
                                        &hub_phys_settings, &hub_pi_cfg, &class_bands, price_mult);
              }
            }
          }
          if (!ready) notice = why, notice_until = vehicle.time() + 3;
        };
        auto collect_next_wristband = [&]() -> bool {
          if (!have_career_catalog) return false;
          const int pending = career_catalog.pending_wristband(career.wristband_points, collected_wristband);
          if (pending < 0 || size_t(pending) >= career_catalog.wristbands.size()) return false;
          collected_wristband = pending;
          const auto& wb = career_catalog.wristbands[size_t(pending)];
          int newly_unlocked = 0;
          for (const auto& ev : career_catalog.events)
            if (ev.style == 1 && ev.level == pending) ++newly_unlocked;
          std::string cname = wb.colour_key;
          for (char& ch : cname) ch = char(std::toupper(static_cast<unsigned char>(ch)));
          ceremony = carview::CeremonyHud{};
          ceremony.active = true;
          ceremony.headline = "NEW WRISTBAND EARNED!";
          ceremony.band_name = cname + " WRISTBAND";
          ceremony.band_colour = cname;
          ceremony.subtitle = std::to_string(newly_unlocked) + " NEW FESTIVAL EVENTS UNLOCKED!";
          if (wb.reward_car_id > 0) {
            ceremony.bonus_car = award_garage_car(wb.reward_car_id, 0, false);
            garage_picker();
          }
          save_due = true;
          return true;
        };
        auto open_events_browser = [&](int hub_filter) {
          if (!have_career_catalog) {
            notice = "career catalog unavailable";
            notice_until = vehicle.time() + 3;
            return;
          }
          int64_t cid = 0;
          int clid = 2;
          current_car_info(cid, clid);
          events_page.open(career_catalog, career, collected_wristband, current_pop_rank(), cid, clid, class_bands,
                           hub_filter);
        };
        auto try_enter_event = [&](const race::CareerEvent& ev) {
          int64_t cid = 0;
          int clid = 2;
          current_car_info(cid, clid);
          const auto elig = career_catalog.check_eligibility(ev, cid, clid, class_name_list(),
                                                             slice12.trainer.effective_any_car());
          if (!elig.eligible) {
            // Check if another car in the player's garage qualifies.
            std::string hint;
            if (have_garage) {
              for (const auto& gc : career_garage.cars) {
                const int gcls = class_bands.empty() ? gc.class_id : garage::class_of(gc.performance_index, class_bands);
                if (career_catalog.check_eligibility(ev, gc.car_id, gcls, class_name_list()).eligible) {
                  if (auto it = baked_ids.find(gc.car_id); it != baked_ids.end()) {
                    hint = "  (TRY " + carview::car_name(it->second) + " IN GARAGE)";
                    break;
                  }
                }
              }
            }
            notice = elig.reason + hint;
            notice_until = vehicle.time() + 4.5;
            return false;
          }
          if (elig.uses_loaner && elig.loaner_car_id > 0) {
            if (auto it = baked_ids.find(elig.loaner_car_id); it != baked_ids.end()) {
              pre_loaner_car = car;
              pre_loaner_garage_id = career_garage.selected;
              car = it->second;
            }
          }
          events_page.close();
          next_event = ev.horizon_id;
          running = false;
          return true;
        };
        auto start_new_career = [&]() {
          career = race::CareerRecord{};
          career.credits = 0;
          career.wristband_points = 0;
          collected_wristband = 0;
          free_roam.reset();
          progress = freeroam::Progress{};
          progress_loaded = false;
          saved = drive::DriveSave{};
          saved.career_started = true;
          saved.intro_completed = false;
          saved.owned_garage = true;
          saved.collected_wristband = 0;
          saved.difficulty = int(race_options.difficulty);
          have_saved = true;
          if (have_skills_data) {
            skill_tracker = std::make_unique<freeroam::SkillTracker>(skills_data, 0.0, 0, 0);
            skill_tracker->reset_session_clock();
          }
          max_skills_car = 0;
          career_garage = garage::Garage{};
          have_garage = true;
          award_garage_car(race::kStarterCarId, race::kStarterCarColour, true);
          garage_picker();
          // Spawn inside the Horizon Festival grounds near Race Central & FESTIVAL_SECOND.
          if (const auto* rc = have_career_catalog ? career_catalog.find_site(race::FestivalSite::kRaceCentral) : nullptr) {
            spawn[0] = rc->pos[0] - 40.0;
            spawn[1] = rc->pos[1];
            spawn[2] = rc->pos[2] - 45.0;
            spawn[3] = 3.14;
          } else {
            spawn[0] = -890.0, spawn[1] = -8.0, spawn[2] = -305.0, spawn[3] = 3.14;
          }
          have_spawn = true;
          show_title = false;
          title_menu_phase = false;
          title_confirm_new = false;
          race.reset();
          race_event.clear();
          pre_loaner_car.clear();
          next_car = race::kStarterCarMedia;
          if (!baked_ids.count(race::kStarterCarId) && !baked_cars.empty()) next_car = baked_cars[0];
          skip_end_save = true;
          new_career_save_pending = true;
          running = false;
        };
        // The pause map: M, or the pause menu's item; the drive waits while
        // it is open.
        bool map_open = false, map_dirty = false, map_dragging = false;
        double map_zoom = 2, map_cx = 0, map_cz = 0;
        MapFilter map_filter = MapFilter::kAll;
        int map_w = 0, map_h = 0;
        auto map_marker_visible = [&](const MapMarker& m) -> bool {
          switch (map_filter) {
            case MapFilter::kAll: return true;
            case MapFilter::kEvents: return m.kind == MapMarker::kEvent || m.kind == MapMarker::kFestivalSite;
            case MapFilter::kStreetRaces: return m.kind == MapMarker::kStreetHub || m.icon_tag == "streetrace";
            case MapFilter::kOutposts: return m.kind == MapMarker::kOutpost || m.kind == MapMarker::kFestivalSite;
            case MapFilter::kCollectibles: return false;
            case MapFilter::kPrStunts: return m.kind == MapMarker::kPrStunt || m.kind == MapMarker::kOutpost;
            default: return true;
          }
        };
        auto update_map_selection = [&]() {
          map_selected_marker = -1;
          map_overlay = carview::MapOverlayHud{};
          if (!map_open) return;
          map_overlay.active = true;
          map_overlay.active_filter = int(map_filter);
          map_overlay.filter_tabs.clear();
          for (int fi = 0; fi < int(MapFilter::kCount); ++fi)
            map_overlay.filter_tabs.push_back(map_filter_name(MapFilter(fi)));
          map_overlay.roads_discovered = road_discovery.ready() ? road_discovery.discovered_count() : 0;
          map_overlay.roads_total = road_discovery.ready() ? road_discovery.total_roads() : 216;
          map_overlay.zoom = map_zoom;
          map_overlay.treasure_map = reveal || treasure_map_setting || slice12.trainer.effective_reveal_collectibles();
          slice12.trainer.note_map_road({map_cx, 0.0, map_cz});
          const double snap_r = 280.0 / std::max(0.5, map_zoom);
          double best_d = snap_r;
          for (size_t i = 0; i < cached_map_markers.size(); ++i) {
            if (!map_marker_visible(cached_map_markers[i])) continue;
            const double d = std::hypot(cached_map_markers[i].x - map_cx, cached_map_markers[i].z - map_cz);
            if (d < best_d) best_d = d, map_selected_marker = int(i);
          }
          const drive::Vec3 at = vehicle.position();
          if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
            const auto& m = cached_map_markers[size_t(map_selected_marker)];
            map_overlay.title = m.title;
            map_overlay.subtitle = m.subtitle;
            map_overlay.distance_m = std::hypot(m.x - at.x, m.z - at.z);
            if (m.kind == MapMarker::kEvent) map_overlay.action_hint = "ENTER EVENT";
            else if (m.kind == MapMarker::kOutpost)
              map_overlay.action_hint = (m.discovered || slice12.trainer.effective_fast_travel_anywhere())
                                            ? "FAST TRAVEL / HUB"
                                            : "SET GPS ROUTE";
            else if (m.kind == MapMarker::kPrStunt) map_overlay.action_hint = "START PR STUNT";
            else map_overlay.action_hint = "ENTER";
          } else {
            map_overlay.title = "COLORADO MAP";
            map_overlay.subtitle = "PAN TO ANY ROAD, EVENT OR FESTIVAL SITE";
            map_overlay.distance_m = std::hypot(map_cx - at.x, map_cz - at.z);
            map_overlay.action_hint = "FAST TRAVEL HERE";
          }
          map_overlay.gps_active = gps_nav.active();
          map_overlay.gps_hint = gps_nav.active() ? "SET / CLEAR GPS (G)" : "SET GPS ROUTE (G)";
        };
        auto open_map = [&]() {
          if (!map_view.ready()) {
            notice = "no map (see the console)";
            notice_until = vehicle.time() + 3;
            return;
          }
          map_open = map_dirty = true;
          picking = false;
          map_zoom = 2;
          map_cx = vehicle.position().x;
          map_cz = vehicle.position().z;
          for (int layer : {0, 1, 3}) renderer.set_overlay({}, 0, 0, 0, 0, layer);
          mm_size = 0;
          radio_popup_gpu = {};
          cached_map_markers = map_scene().markers;
          update_map_selection();
          save_now("map");
        };
        auto close_map = [&]() {
          map_open = false;
          map_overlay.active = false;
          renderer.set_overlay({}, 0, 0, 0, 0, 2);
          minimap_at = -1;
        };
        auto sync_pause_labels = [&]() {
          const int pending =
              have_career_catalog ? career_catalog.pending_wristband(career.wristband_points, collected_wristband) : -1;
          if (pending >= 0 && size_t(pending) < career_catalog.wristbands.size()) {
            std::string cn = career_catalog.wristbands[size_t(pending)].colour_key;
            for (char& ch : cn) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            menu.events_label = "COLLECT " + cn + " WRISTBAND!";
          } else {
            menu.events_label = "EVENTS";
          }
          char db[64];
          std::snprintf(db, sizeof db, "< DIFFICULTY: %s (+%d%%) >", difficulty_name(race_options.difficulty),
                        race::difficulty_bonus_percent(int(race_options.difficulty)));
          menu.difficulty_label = db;
          menu.in_rally_world = !colorado_world();
        };
        auto cycle_difficulty = [&](int by) {
          const int d = ((int(race_options.difficulty) + by) % 5 + 5) % 5;
          race_options.difficulty = race::Difficulty(d);
          if (race) race->set_difficulty(race_options.difficulty);
          sync_pause_labels();
        };
        // The chosen pause menu action.
        auto choose = [&]() {
          const PauseMenu::Action act = menu.selected_action();
          menu.open = false;
          if (act == PauseMenu::kMap) open_map();
          if (act == PauseMenu::kEvents) open_events_browser(0);
          if (act == PauseMenu::kGarage) {
            open_garage_browser();
          }
          if (act == PauseMenu::kUpgrades) open_shop_or_autoshow(false);
          if (act == PauseMenu::kAutoshow) open_shop_or_autoshow(true);
          if (act == PauseMenu::kPaint) open_paint();
          if (act == PauseMenu::kRally) {
            if (!rally_catalog_tried && !state_root.empty()) {
              rally_catalog_tried = true;
              const auto db_ptr = parts_db.valid() ? parts_db.get() : std::shared_ptr<cars::PartsDb>{};
              std::string rerr;
              if (race::load_rally_catalog(state_root, db_ptr.get(), nullptr, rally_catalog, &rerr)) {
                const auto cd_cache = save_path.empty() ? (std::filesystem::path("out") / "audio_cache")
                                                        : (save_path.parent_path() / "audio_cache");
                audio_host.init_codriver(rally_catalog.package_dir, cd_cache, language);
              } else {
                std::fprintf(stderr, "rally: catalog load failed: %s\n", rerr.c_str());
              }
            }
            if (!rally_catalog.loaded || rally_catalog.package_dir.empty()) {
              notice = "Horizon Rally expansion package not installed";
              notice_until = vehicle.time() + 4;
            } else if (colorado_world()) {
              const drive::Vec3 vp = vehicle.position();
              const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
              colorado_return_pos = {vp.x, vp.y, vp.z};
              colorado_return_heading = std::atan2(vf.x, vf.z);
              has_colorado_return = true;
              rally_world_switch = +1;
              running = false;
            } else {
              rally_hub_open = true;
              rally_hub_selected = 0;
              rally_notice.clear();
            }
          }
          if (act == PauseMenu::kClub) {
            club_check();
            club_open = true;
            club_pick = 0;
          }
          if (act == PauseMenu::kAchievements) {
            eval_achievements(false);
            achievements.open_screen();
          }
          if (act == PauseMenu::kPhotoMode) {
            const drive::Vec3 pat = vehicle.position();
            const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                             carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
          }
          if (act == PauseMenu::kReplay) open_replay_viewer(true);
          if (act == PauseMenu::kSettings) {
            settings_page.load(read_settings(settings_path(), nullptr));
            settings_page.open();
          }
          if (act == PauseMenu::kCareer) {
            std::vector<cars::PackInfo> packs;
            const carview::CareerSummary cs = career_summary(packs);
            career_page.open(cs, std::move(packs));
          }
          if (act == PauseMenu::kReset) put_back();
          if (act == PauseMenu::kRestartRace && race) {
            club_pending.event_restarted = true;
            club_step();
            results_page = 0;
            next_event = race_event;
            running = false;
          }
          if (act == PauseMenu::kQuitRace && race) {
            results_page = 0;
            leave_race = true;
            running = false;
          }
          if (act == PauseMenu::kQuitToTitle) {
            save_now("title");
            show_title = true;
            title_menu_phase = true;
            title_confirm_new = false;
            title_selected = 0;
          }
          if (act == PauseMenu::kQuit) running = false;
        };
        // After the player's finish (SPEC-EVENT-REWARDS, Results screen):
        // FINISHED (0) -> CASH REWARD (1) -> POINTS EARNED (2) -> NEW CAR WON (3) -> free roam.
        auto race_continue = [&]() {
          if (results_table) return;  // the table takes only B
          if (results_page == 0) {
            results_page = 1;
            return;
          }
          if (results_page == 1 && results_points) {
            results_page = 2, points_at = vehicle.time();
            return;
          }
          if ((results_page == 1 || results_page == 2) && !won_car_name.empty()) {
            results_page = 3;
            return;
          }
          results_page = 0;
          if (!race->post_race_spawn(spawn)) {
            const drive::Vec3 at = vehicle.position();
            const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
            spawn[0] = at.x, spawn[1] = at.y, spawn[2] = at.z, spawn[3] = std::atan2(f.x, f.z);
          }
          have_spawn = true;
          leave_race = true;
          running = false;
        };
        auto race_restart = [&]() {
          if (results_table) return;
          results_page = 0;
          next_event = race_event;
          running = false;
        };
        auto open_menu = [&]() {
          sync_pause_labels();
          menu.show(race != nullptr);
          picking = false;
          save_now("pause");
        };
        auto activate_world_trigger = [&]() -> bool {
          if (race || !active_trigger.in_range) return false;
          if (active_trigger.kind == carview::WorldTrigger::kFestivalSite && active_trigger.site) {
            switch (active_trigger.site->kind) {
              case race::FestivalSite::kRaceCentral:
                if (!collect_next_wristband()) open_events_browser(0);
                return true;
              case race::FestivalSite::kAutoshow: open_shop_or_autoshow(true); return true;
              case race::FestivalSite::kWorkshop: open_shop_or_autoshow(false); return true;
              case race::FestivalSite::kPaintShop: open_paint(); return true;
            }
          } else if (active_trigger.kind == carview::WorldTrigger::kStreetHub && active_trigger.hub) {
            open_events_browser(active_trigger.hub->id);
            return true;
          } else if (active_trigger.kind == carview::WorldTrigger::kEvent && active_trigger.event) {
            try_enter_event(*active_trigger.event);
            return true;
          } else if (active_trigger.kind == carview::WorldTrigger::kOutpost && active_trigger.outpost) {
            open_outpost_hub(active_trigger.outpost->id);
            return true;
          }
          return false;
        };
        while (running) {
          // The points page auto-advances after 4 s (Enter/A also advances early).
          if (race && results_page == 2 && vehicle.time() - points_at > 4.0) race_continue();
          SDL_Event e;
          while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            // macOS Command shortcuts (Cmd+,, Ctrl+Cmd+F, Cmd+Shift+P, Cmd+T, Cmd+Q).
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
#if defined(__APPLE__)
              constexpr bool kIsMacOs = true;
#else
              constexpr bool kIsMacOs = false;
#endif
              const auto sc = carview::match_macos_shortcut(e.key.key, e.key.mod, kIsMacOs);
              if (sc == carview::PlatformShortcut::kQuit) {
                running = false;
                continue;
              } else if (sc == carview::PlatformShortcut::kSettings) {
                settings_page.load(read_settings(settings_path(), nullptr));
                settings_page.open();
                settings_live_overlay = true;
                audio_host.trigger_ui(audio::UiCue::kMenuOpen);
                continue;
              } else if (sc == carview::PlatformShortcut::kToggleFullscreen) {
                const bool fs = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(window, !fs);
                continue;
              } else if (sc == carview::PlatformShortcut::kPhotoMode && !picking && !(race && race->player_finished())) {
                const drive::Vec3 pat = vehicle.position();
                const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                                 carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
                continue;
              } else if (sc == carview::PlatformShortcut::kTrainerOrTime) {
                tod_clock.step_hours(1.0);
                if (traffic) traffic->set_hour(tod_clock.hour());
                notice = "MACOS CMD+T: STEPPED TIME +1H";
                notice_until = vehicle.time() + 2.5;
                continue;
              }
            }
            // Android / touchscreen multi-touch driving controls.
            if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
                e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
              touch_controls.handle_sdl_event(e, float(width), float(height), vehicle.time());
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
              if (e.key.key == SDLK_F3) {
                perf_overlay_open = !perf_overlay_open;
                renderer.set_gpu_timing(perf_overlay_open || inspector_open);
                continue;
              }
              if (e.key.key == SDLK_F8 && (e.key.mod & SDL_KMOD_CTRL) != 0) {
                inspector_open = !inspector_open;
                renderer.set_inspector_preview(inspector_open, inspector_view);
                renderer.set_gpu_timing(perf_overlay_open || inspector_open);
                if (!inspector_open) has_inspector_preview = false;
                continue;
              }
              if (inspector_open) {
                int next_v = -1;
                if (e.key.key == SDLK_LEFTBRACKET)
                  next_v = (int(inspector_view) + int(carview::InspectorView::kCount) - 1) %
                           int(carview::InspectorView::kCount);
                else if (e.key.key == SDLK_RIGHTBRACKET)
                  next_v = (int(inspector_view) + 1) % int(carview::InspectorView::kCount);
                else if (e.key.key >= SDLK_1 && e.key.key <= SDLK_6)
                  next_v = int(e.key.key - SDLK_1);
                if (next_v >= 0) {
                  inspector_view = static_cast<carview::InspectorView>(next_v);
                  renderer.set_inspector_preview(true, inspector_view);
                  continue;
                }
              }
            }
            if (skip_vo_with_a && !show_title && !menu.open && !settings_page.is_open() && !map_open &&
                !fmv_player.active() && audio_host.telemetry().vo_playing) {
              const bool pad_a =
                  e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH;
              if (pad_a) audio_host.skip_vo();
            }
            if (fmv_player.active()) {
              const bool skip_key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                                    (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE || e.key.key == SDLK_ESCAPE);
              const bool skip_pad = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                    (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                                     e.gbutton.button == SDL_GAMEPAD_BUTTON_START);
              if (skip_key || skip_pad) {
                fmv_player.skip();
                audio_host.set_fmv_audio(nullptr, nullptr);
              }
              continue;
            }
            if (story_intro.active() && e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_TAB) {
              const auto sa = story_intro.skip_stage();
              if (sa.play_fmv) {
                const auto mp = carview::FmvPlayer::resolve_movie(game_root_of(), sa.fmv_id);
                if (!mp.empty() && fmv_player.open(mp, language)) {
                  audio_host.set_fmv_audio(
                      [](void* ctx, float* out, int fr) { return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, fr); },
                      &fmv_player);
                }
              }
              if (sa.open_autoshow) open_shop_or_autoshow(true);
              if (sa.intro_finished) saved.intro_completed = true;
              continue;
            }
            // 1. Title Screen input (when settings isn't open on top of it).
            if (show_title && !settings_page.is_open()) {
              const bool has_save_opt =
                  have_saved && (saved.career_started || saved.owned_garage || career.num_races > 0 || career.credits > 0);
              const int item_count = has_save_opt ? 5 : 4;
              auto confirm_title = [&]() {
                if (!title_menu_phase) {
                  title_menu_phase = true;
                  title_selected = 0;
                  return;
                }
                if (title_confirm_new) {
                  if (title_confirm_selected == 0) start_new_career();
                  else title_confirm_new = false;
                  return;
                }
                const int idx = has_save_opt ? title_selected : (title_selected + 1);
                if (idx == 0) {  // CONTINUE CAREER
                  show_title = false;
                } else if (idx == 1) {  // NEW CAREER
                  if (has_save_opt) {
                    title_confirm_new = true;
                    title_confirm_selected = 1;
                  } else {
                    start_new_career();
                  }
                } else if (idx == 2) {  // FREE ROAM
                  show_title = false;
                } else if (idx == 3) {  // SETTINGS
                  settings_page.load(read_settings(settings_path(), nullptr));
                  settings_page.open();
                } else if (idx == 4) {  // QUIT
                  running = false;
                }
              };
              auto back_title = [&]() {
                if (title_confirm_new) title_confirm_new = false;
                else if (title_menu_phase) title_menu_phase = false;
                else running = false;
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) confirm_title();
                else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) back_title();
                else if (title_confirm_new && (k == SDLK_UP || k == SDLK_W || k == SDLK_DOWN || k == SDLK_S))
                  title_confirm_selected = 1 - title_confirm_selected;
                else if (title_menu_phase && (k == SDLK_UP || k == SDLK_W))
                  title_selected = (title_selected + item_count - 1) % item_count;
                else if (title_menu_phase && (k == SDLK_DOWN || k == SDLK_S))
                  title_selected = (title_selected + 1) % item_count;
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_START) confirm_title();
                else if (b == SDL_GAMEPAD_BUTTON_EAST) back_title();
                else if (title_confirm_new && (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN))
                  title_confirm_selected = 1 - title_confirm_selected;
                else if (title_menu_phase && b == SDL_GAMEPAD_BUTTON_DPAD_UP)
                  title_selected = (title_selected + item_count - 1) % item_count;
                else if (title_menu_phase && b == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                  title_selected = (title_selected + 1) % item_count;
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // 2. Wristband Collection Ceremony overlay.
            if (ceremony.active) {
              if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                   (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                    e.key.key == SDLK_ESCAPE)) ||
                  (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                   (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST))) {
                ceremony.active = false;
                // Refresh Events page if it was open underneath.
                if (events_page.is_open()) open_events_browser(events_page.hub_filter());
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // 3. Pre-Race Grid Menu (race-grid.png).
            if (race && grid_menu_open && !menu.open && !settings_page.is_open()) {
              auto confirm_grid = [&]() {
                if (grid_menu_selected == 0) {  // START RACE
                  grid_menu_open = false;
                } else if (grid_menu_selected == 1) {  // DIFFICULTY
                  cycle_difficulty(+1);
                } else {  // QUIT RACE
                  grid_menu_open = false;
                  leave_race = true;
                  running = false;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_UP || k == SDLK_W) grid_menu_selected = (grid_menu_selected + 2) % 3;
                else if (k == SDLK_DOWN || k == SDLK_S) grid_menu_selected = (grid_menu_selected + 1) % 3;
                else if (grid_menu_selected == 1 && (k == SDLK_LEFT || k == SDLK_A)) cycle_difficulty(-1);
                else if (grid_menu_selected == 1 && (k == SDLK_RIGHT || k == SDLK_D)) cycle_difficulty(+1);
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) confirm_grid();
                else if (k == SDLK_ESCAPE) {
                  grid_menu_open = false;
                  leave_race = true;
                  running = false;
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) grid_menu_selected = (grid_menu_selected + 2) % 3;
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) grid_menu_selected = (grid_menu_selected + 1) % 3;
                else if (grid_menu_selected == 1 && b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) cycle_difficulty(-1);
                else if (grid_menu_selected == 1 && b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) cycle_difficulty(+1);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) confirm_grid();
                else if (b == SDL_GAMEPAD_BUTTON_EAST) {
                  grid_menu_open = false;
                  leave_race = true;
                  running = false;
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // 4. Interactive Pause Map (pan, zoom, and Enter/A to Enter Event or Fast Travel!).
            if (map_open) {
              bool close = false;
              const double pan = 1200 / map_zoom;
              auto zoom_by = [&](double f) {
                map_zoom = std::clamp(map_zoom * f, 1.0, 24.0);
                map_dirty = true;
                update_map_selection();
              };
              auto pan_by = [&](double dx, double dz) {
                map_cx += dx, map_cz += dz;
                map_dirty = true;
                update_map_selection();
              };
              auto map_activate = [&]() {
                if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
                  const MapMarker m = cached_map_markers[size_t(map_selected_marker)];
                  close_map();
                  if (m.kind == MapMarker::kEvent) {
                    if (const auto* ev = career_catalog.find_event(m.id)) try_enter_event(*ev);
                    return;
                  }
                  if (m.kind == MapMarker::kFestivalSite) {
                    const auto kind = race::FestivalSite::Kind(m.site_or_hub);
                    if (kind == race::FestivalSite::kRaceCentral) {
                      if (!collect_next_wristband()) open_events_browser(0);
                    } else if (kind == race::FestivalSite::kAutoshow) open_shop_or_autoshow(true);
                    else if (kind == race::FestivalSite::kWorkshop) open_shop_or_autoshow(false);
                    else if (kind == race::FestivalSite::kPaintShop) open_paint();
                  } else if (m.kind == MapMarker::kStreetHub) {
                    open_events_browser(m.site_or_hub);
                  } else if (m.kind == MapMarker::kOutpost && have_outposts) {
                    if (const auto* op = outposts.find_outpost(m.id)) {
                      const drive::Vec3 at = vehicle.position();
                      if (std::hypot(op->trigger.x - at.x, op->trigger.z - at.z) <= std::max(180.0, op->discovery_radius)) {
                        open_outpost_hub(op->id);
                      } else if (outpost_progress.is_outpost_discovered(op->id) ||
                                 slice12.trainer.effective_fast_travel_anywhere()) {
                        if (race != nullptr) {
                          notice = "QUIT RACE TO FAST TRAVEL";
                          notice_until = vehicle.time() + 3.0;
                        } else {
                          spawn[0] = op->spawn.x;
                          spawn[1] = 0.0;
                          spawn[2] = op->spawn.z;
                          spawn[3] = std::atan2(op->spawn.z_axis[0], op->spawn.z_axis[2]);
                          have_spawn = true;
                          fast_travel = true;
                          next_car = car;
                          running = false;
                        }
                      } else if (gps_solver.ready()) {
                        fire_satnav_event(gps_nav.set_destination(op->trigger.x, op->trigger.z,
                                                                  traffic::GpsDestKind::kOutpost, op->display_name,
                                                                  at.x, at.z));
                        notice = "DRIVE TO OUTPOST TO UNLOCK FAST TRAVEL";
                        notice_until = vehicle.time() + 3.5;
                      }
                    }
                  } else if (m.kind == MapMarker::kPrStunt && have_outposts) {
                    if (const auto* st = outposts.find_stunt(m.id)) {
                      if (race == nullptr) start_pr_stunt(*st);
                    }
                  }
                  return;
                }
                if (race != nullptr) {
                  close_map();
                  notice = "QUIT RACE TO FAST TRAVEL";
                  notice_until = vehicle.time() + 3.0;
                  return;
                }
                // Fast travel to map cursor location on Colorado.
                close_map();
                spawn[0] = map_cx, spawn[1] = 0.0, spawn[2] = map_cz, spawn[3] = 0.0;
                have_spawn = true;
                fast_travel = true;
                next_car = car;
                running = false;
              };
              auto map_set_gps = [&]() {
                if (!gps_solver.ready() || race != nullptr) return;
                const drive::Vec3 at = vehicle.position();
                double tx = map_cx, tz = map_cz;
                std::string label = "CUSTOM WAYPOINT";
                traffic::GpsDestKind dk = traffic::GpsDestKind::kGeneric;
                if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
                  const auto& m = cached_map_markers[size_t(map_selected_marker)];
                  tx = m.x, tz = m.z;
                  label = m.title;
                  if (m.kind == MapMarker::kEvent) {
                    dk = m.icon_tag == "exhibition" ? traffic::GpsDestKind::kShowcase
                                                    : traffic::GpsDestKind::kFestivalRace;
                  } else if (m.kind == MapMarker::kStreetHub) {
                    dk = traffic::GpsDestKind::kStreetRace;
                  } else if (m.kind == MapMarker::kOutpost) {
                    dk = traffic::GpsDestKind::kOutpost;
                  } else if (m.kind == MapMarker::kFestivalSite) {
                    const auto sk = race::FestivalSite::Kind(m.site_or_hub);
                    if (sk == race::FestivalSite::kRaceCentral) dk = traffic::GpsDestKind::kRaceCentral;
                    else if (sk == race::FestivalSite::kAutoshow) dk = traffic::GpsDestKind::kAutoshow;
                    else if (sk == race::FestivalSite::kWorkshop) dk = traffic::GpsDestKind::kWorkshop;
                    else if (sk == race::FestivalSite::kPaintShop) dk = traffic::GpsDestKind::kPaintShop;
                  }
                }
                // Toggle off if pressing GPS again on the existing destination.
                if (gps_nav.active() && std::hypot(tx - gps_nav.route().dest_x, tz - gps_nav.route().dest_z) < 120.0) {
                  gps_nav.clear();
                  audio_host.trigger_satnav(audio::SatNavCue::kDestReset);
                } else {
                  const auto gev = gps_nav.set_destination(tx, tz, dk, label, at.x, at.z);
                  fire_satnav_event(gev);
                }
                map_dirty = true;
                update_map_selection();
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (!e.key.repeat && (k == SDLK_M || k == SDLK_ESCAPE)) close = true;
                if (!e.key.repeat && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)) map_activate();
                if (!e.key.repeat && k == SDLK_G) map_set_gps();
                if (!e.key.repeat && k == SDLK_C && gps_nav.active()) {
                  gps_nav.clear();
                  audio_host.trigger_satnav(audio::SatNavCue::kDestReset);
                  map_dirty = true;
                  update_map_selection();
                }
                if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) zoom_by(1.35);
                if (k == SDLK_MINUS || k == SDLK_KP_MINUS) zoom_by(1 / 1.35);
                if (!e.key.repeat && k == SDLK_Q) {
                  map_filter = MapFilter((int(map_filter) + int(MapFilter::kCount) - 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                }
                if (!e.key.repeat && (k == SDLK_E || k == SDLK_TAB)) {
                  map_filter = MapFilter((int(map_filter) + 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                }
                if (k == SDLK_LEFT || k == SDLK_A) pan_by(-pan * 0.45, 0);
                if (k == SDLK_RIGHT || k == SDLK_D) pan_by(pan * 0.45, 0);
                if (k == SDLK_UP || k == SDLK_W) pan_by(0, pan * 0.45);
                if (k == SDLK_DOWN || k == SDLK_S) pan_by(0, -pan * 0.45);
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) zoom_by(1.22);
                else if (e.wheel.y < 0) zoom_by(1.0 / 1.22);
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                map_dragging = true;
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                map_dragging = false;
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION && map_dragging && map_w > 64 && map_h > 64) {
                const double mpp = std::max(freeroam::kMapWidth / map_w, freeroam::kMapHeight / map_h) / map_zoom;
                pan_by(-double(e.motion.xrel) * mpp, double(e.motion.yrel) * mpp);
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_BACK)
                  close = true;
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) map_activate();
                if (b == SDL_GAMEPAD_BUTTON_WEST) map_set_gps();
                if (b == SDL_GAMEPAD_BUTTON_NORTH && gps_nav.active()) {
                  gps_nav.clear();
                  audio_host.trigger_satnav(audio::SatNavCue::kDestReset);
                  map_dirty = true;
                  update_map_selection();
                }
                if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  map_filter = MapFilter((int(map_filter) + int(MapFilter::kCount) - 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                }
                if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  map_filter = MapFilter((int(map_filter) + 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) pan_by(-pan * 0.45, 0);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) pan_by(pan * 0.45, 0);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) pan_by(0, pan * 0.45);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) pan_by(0, -pan * 0.45);
              }
              if (close) {
                map_dragging = false;
                close_map();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // 5. Race Central / Events / Street Hub browser.
            if (events_page.is_open()) {
              auto confirm_event = [&]() {
                const auto* sel = events_page.selected_entry();
                if (!sel) return;
                if (sel->is_collect_wristband) {
                  collect_next_wristband();
                  return;
                }
                if (!sel->unlocked) {
                  notice = sel->reason;
                  notice_until = vehicle.time() + 3.5;
                  return;
                }
                if (sel->event) try_enter_event(*sel->event);
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) {
                  events_page.close();
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_UP || k == SDLK_W) {
                  events_page.move(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_DOWN || k == SDLK_S) {
                  events_page.move(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_Q) {
                  events_page.switch_tab(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_E) {
                  events_page.switch_tab(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm_event();
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  events_page.close();
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  events_page.move(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  events_page.move(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  events_page.switch_tab(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  events_page.switch_tab(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm_event();
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // 5b. Regional Horizon Outpost Hub menu.
            if (outpost_hub_open) {
              const size_t n = outpost_hub_items.size();
              auto confirm_outpost_item = [&]() {
                if (!n || outpost_hub_pick >= n) return;
                const auto item = outpost_hub_items[outpost_hub_pick];
                outpost_hub_open = false;
                switch (item.kind) {
                  case OutpostHubItem::kPrStunt:
                    if (const auto* st = outposts.find_stunt(item.ref_id)) start_pr_stunt(*st);
                    break;
                  case OutpostHubItem::kGarage:
                    open_garage_browser();
                    break;
                  case OutpostHubItem::kNearbyEvents:
                    open_events_browser(0);
                    break;
                  case OutpostHubItem::kFastTravelFestival:
                  case OutpostHubItem::kFastTravelOutpost:
                    if (race != nullptr) {
                      notice = "QUIT RACE TO FAST TRAVEL";
                      notice_until = vehicle.time() + 3.0;
                    } else {
                      spawn[0] = item.dest_x;
                      spawn[1] = 0.0;
                      spawn[2] = item.dest_z;
                      spawn[3] = 0.0;
                      have_spawn = true;
                      fast_travel = true;
                      next_car = car;
                      running = false;
                    }
                    break;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) outpost_hub_open = false;
                else if (n && (k == SDLK_UP || k == SDLK_W)) outpost_hub_pick = (outpost_hub_pick + n - 1) % n;
                else if (n && (k == SDLK_DOWN || k == SDLK_S)) outpost_hub_pick = (outpost_hub_pick + 1) % n;
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat)
                  confirm_outpost_item();
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) outpost_hub_open = false;
                else if (n && b == SDL_GAMEPAD_BUTTON_DPAD_UP) outpost_hub_pick = (outpost_hub_pick + n - 1) % n;
                else if (n && b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) outpost_hub_pick = (outpost_hub_pick + 1) % n;
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) confirm_outpost_item();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // 6. Garage car browser (horizontal carousel + Sort/Filter/Sell Car).
            if (garage_open) {
              const bool use_browser = have_garage && !career_garage.cars.empty();
              const size_t n = use_browser ? garage_browser.visible().size() : baked_cars.size();
              auto move_garage = [&](int by) {
                garage_sell_confirm_id = -1;
                if (use_browser) {
                  if (n) garage_browser.move(by);
                } else if (n) {
                  garage_pick = size_t(((int(garage_pick) + by) % int(n) + int(n)) % int(n));
                }
                audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto drive_selected = [&]() {
                garage_sell_confirm_id = -1;
                if (use_browser) {
                  const auto* e = garage_browser.selected_entry();
                  if (!e) return;
                  garage_open = false;
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                  std::string target_media = e->media_name;
                  if (target_media.empty() && baked_ids.count(e->car_id)) target_media = baked_ids.at(e->car_id);
                  if (target_media.empty()) return;
                  career_garage.selected = e->garage_id;
                  write_career_garage();
                  const drive::Vec3 at = vehicle.position();
                  const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
                  spawn[0] = at.x;
                  spawn[1] = at.y;
                  spawn[2] = at.z;
                  spawn[3] = std::atan2(fwd.x, fwd.z);
                  have_spawn = true;
                  next_car = target_media;
                  running = false;
                  return;
                }
                if (!n || garage_pick >= baked_cars.size()) return;
                garage_open = false;
                audio_host.trigger_ui(audio::UiCue::kAccept);
                pick = garage_pick;
                picking = true;
                take_pick();
              };
              auto try_sell = [&]() {
                if (!use_browser) return;
                const auto* se = garage_browser.selected_entry();
                if (!se) return;
                if (se->is_current) {
                  notice = "SWITCH CARS BEFORE SELLING YOUR ACTIVE CAR";
                  notice_until = vehicle.time() + 3.0;
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                  return;
                }
                if (career_garage.cars.size() <= 1) {
                  notice = "CANNOT SELL YOUR LAST CAR";
                  notice_until = vehicle.time() + 3.0;
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                  return;
                }
                if (garage_sell_confirm_id != se->garage_id) {
                  garage_sell_confirm_id = se->garage_id;
                  notice = "PRESS [LB] / DEL AGAIN TO SELL FOR +" + carview::thousands(se->sell_cr) + " Cr";
                  notice_until = vehicle.time() + 4.0;
                  audio_host.trigger_ui(audio::UiCue::kMove);
                  return;
                }
                garage_sell_confirm_id = -1;
                const cars::PartsDb* gdb =
                    (parts_db.valid() && parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                        ? parts_db.get().get()
                        : nullptr;
                std::string smsg;
                if (garage_browser.sell_selected(career_garage, gdb, class_bands, career.credits, smsg)) {
                  garage_picker();
                  write_career_garage();
                  save_now("sell");
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                } else {
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                notice = smsg;
                notice_until = vehicle.time() + 3.5;
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) {
                  garage_open = false;
                  garage_sell_confirm_id = -1;
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) {
                  move_garage(-1);
                } else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) {
                  move_garage(1);
                } else if (k == SDLK_Y && !e.key.repeat && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_sort(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_X && !e.key.repeat && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_class_filter(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if ((k == SDLK_DELETE || k == SDLK_BACKSPACE || k == SDLK_Q) && !e.key.repeat) {
                  try_sell();
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  drive_selected();
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  garage_open = false;
                  garage_sell_confirm_id = -1;
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  move_garage(-1);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  move_garage(1);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_sort(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_WEST && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_class_filter(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  try_sell();
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  drive_selected();
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // The upgrade shop takes the keys and buttons while open; leaving it
            // after a change rebuilds the car where it stands.
            if (shop.is_open()) {
              auto leave_shop = [&]() {
                shop.close();
                if (!shop.changed()) return;
                write_career_garage();
                save_now("upgrade");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                spawn[0] = at.x, spawn[1] = at.y, spawn[2] = at.z, spawn[3] = std::atan2(f.x, f.z);
                have_spawn = true;
                next_car = car;
                running = false;
              };
              auto fit = [&]() {
                std::string message;
                shop.fit(discount(), career.credits, message);
                notice = message;
                notice_until = vehicle.time() + 3;
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) leave_shop();
                if (k == SDLK_UP || k == SDLK_W) shop.move_slot(-1);
                if (k == SDLK_DOWN || k == SDLK_S) shop.move_slot(1);
                if (k == SDLK_LEFT || k == SDLK_A) shop.move_part(-1);
                if (k == SDLK_RIGHT || k == SDLK_D) shop.move_part(1);
                if (k == SDLK_Q) shop.move_category(-1);
                if (k == SDLK_E) shop.move_category(1);
                if (k == SDLK_X) {
                  std::string smsg;
                  shop.sell_shown_part(career.credits, smsg);
                  notice = smsg, notice_until = vehicle.time() + 3;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) fit();
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                  leave_shop();
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) { shop.move_slot(-1); audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { shop.move_slot(1); audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) { shop.move_part(-1); audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) { shop.move_part(1); audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { shop.move_category(-1); audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { shop.move_category(1); audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  std::string smsg;
                  const bool ok = shop.sell_shown_part(career.credits, smsg);
                  audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kCancel);
                  notice = smsg, notice_until = vehicle.time() + 3;
                }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) { audio_host.trigger_ui(audio::UiCue::kAccept); fit(); }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // The Autoshow takes the keys and buttons while open; a purchase
            // drives the new car away from where this one stands.
            if (autoshow.is_open()) {
              auto buy = [&]() {
                const auto db = parts_db.valid() ? parts_db.get() : nullptr;
                std::string media, message = "no parts database";
                const bool bought = db && autoshow.buy(*db, career_garage, career.credits, media, message);
                notice = message;
                notice_until = vehicle.time() + 4;
                if (!bought) return;
                autoshow.close();
                garage_picker();
                write_career_garage();
                save_now("purchase");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                spawn[0] = at.x, spawn[1] = at.y, spawn[2] = at.z, spawn[3] = std::atan2(f.x, f.z);
                have_spawn = true;
                next_car = media;
                running = false;
              };
              auto confirm_step = [&]() {
                if (autoshow.advance_step()) buy();
              };
              auto cancel_step = [&]() {
                if (!autoshow.back_step()) autoshow.close();
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) cancel_step();
                if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                  if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) autoshow.move_make(-1, career.credits);
                  if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) autoshow.move_make(1, career.credits);
                } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                  if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) autoshow.move_colour(-1);
                  if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) autoshow.move_colour(1);
                } else {
                  if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) autoshow.move(-1);
                  if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) autoshow.move(1);
                  if (k == SDLK_Q) autoshow.move_make(-1, career.credits);
                  if (k == SDLK_E) autoshow.move_make(1, career.credits);
                  if (k == SDLK_C) autoshow.set_step(carview::AutoshowStep::kColorSelect);
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) confirm_step();
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) cancel_step();
                if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) autoshow.move_make(-1, career.credits);
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) autoshow.move_make(1, career.credits);
                } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) autoshow.move_colour(-1);
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) autoshow.move_colour(1);
                } else {
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) autoshow.move(-1);
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) autoshow.move(1);
                  if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) autoshow.move_make(-1, career.credits);
                  if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) autoshow.move_make(1, career.credits);
                  if (b == SDL_GAMEPAD_BUTTON_NORTH) autoshow.set_step(carview::AutoshowStep::kColorSelect);
                }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) confirm_step();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // Horizon Rally Hub / Service Area / Championship Results input.
            if (rally_results_open) {
              const bool confirm = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                                    (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                                     e.key.key == SDLK_ESCAPE)) ||
                                   (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                    (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                                     e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST));
              if (confirm) {
                rally_results_open = false;
                rally_session.retire();
                rally_hub_open = true;
                leave_race = true;
                running = false;
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if (rally_service_open) {
              auto act_service = [&]() {
                if (rally_service_selected == 0) {
                  rally_service_open = false;
                  if (rally_session.advance_from_service_area()) {
                    if (const auto* stg = rally_session.current_stage()) {
                      race_options.stage_title = stg->name;
                      next_event = "stage:" + std::to_string(stg->route_id);
                      running = false;
                    }
                  }
                } else if (rally_service_selected == 1) {
                  car_damage.reset();
                  rally_service_repaired = true;
                  rally_notice = "CAR REPAIRED TO 100% — READY FOR NEXT STAGE";
                } else if (rally_service_selected == 2) {
                  rally_service_open = false;
                  open_shop_or_autoshow(false);
                } else if (rally_service_selected == 3) {
                  rally_service_open = false;
                  rally_session.retire();
                  rally_hub_open = true;
                  leave_race = true;
                  running = false;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_UP || e.key.key == SDLK_W) rally_service_selected = (rally_service_selected + 3) % 4;
                else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_S) rally_service_selected = (rally_service_selected + 1) % 4;
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE) act_service();
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) rally_service_selected = (rally_service_selected + 3) % 4;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) rally_service_selected = (rally_service_selected + 1) % 4;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) act_service();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if (rally_hub_open && rally_catalog.loaded) {
              int64_t cid = 0;
              int clid = 4;
              current_car_info(cid, clid);
              const int disc = free_roam ? freeroam::upgrade_discount(free_roam->signs_found()) : 0;
              const auto db_ptr = parts_db.valid() ? parts_db.get() : std::shared_ptr<cars::PartsDb>{};
              const auto hst = carview::make_rally_hub_state(rally_catalog, saved, have_garage ? &career_garage : nullptr,
                                                             db_ptr.get(), class_bands, car, clid, 0, disc,
                                                             rally_hub_selected, rally_notice);
              const int n_items = std::max(1, int(hst.items.size()));
              auto activate_rally = [&]() {
                if (rally_hub_selected < 0 || size_t(rally_hub_selected) >= hst.items.size()) return;
                const auto& it = hst.items[size_t(rally_hub_selected)];
                if (it.kind == carview::RallyHubItem::kHeats) {
                  if (rally_session.start_heats(rally_catalog, int(race_options.difficulty), race_options.seed)) {
                    if (auto bit = baked_ids.find(rally_catalog.heats.player_car_id); bit != baked_ids.end()) {
                      pre_loaner_car = car;
                      pre_loaner_garage_id = career_garage.selected;
                      car = bit->second;
                    }
                    rally_hub_open = false;
                    race_options.stage_title = rally_catalog.heats.name;
                    next_event = "stage:" + std::to_string(rally_catalog.heats.route_id);
                    running = false;
                  }
                } else if (it.kind == carview::RallyHubItem::kChampionship) {
                  if (!it.unlocked || !it.eligible) {
                    rally_notice = it.lock_reason;
                    return;
                  }
                  if (rally_session.start_championship(rally_catalog, it.event_id, int(race_options.difficulty),
                                                       race_options.seed)) {
                    if (const auto* stg = rally_session.current_stage()) {
                      rally_hub_open = false;
                      race_options.stage_title = stg->name;
                      next_event = "stage:" + std::to_string(stg->route_id);
                      running = false;
                    }
                  }
                } else if (it.kind == carview::RallyHubItem::kFitRallyKit) {
                  if (have_garage && db_ptr && db_ptr->is_open()) {
                    if (const garage::Car* sc = career_garage.selected_car()) {
                      const auto plan = garage::plan_rally_kit(career_garage, *db_ptr, sc->id, disc, true,
                                                               &hub_phys_settings, &hub_pi_cfg,
                                                               !class_bands.empty() ? &class_bands : nullptr);
                      std::string msg;
                      if (garage::apply_rally_kit(career_garage, *db_ptr, sc->id, plan, disc, career.credits,
                                                  &hub_phys_settings, &hub_pi_cfg,
                                                  !class_bands.empty() ? &class_bands : nullptr, &msg)) {
                        saved.credits = career.credits;
                        save_now("rally kit");
                        rally_notice = msg;
                        again = true;
                        running = false;
                      } else {
                        rally_notice = msg;
                      }
                    }
                  }
                } else if (it.kind == carview::RallyHubItem::kFreeRoam) {
                  rally_hub_open = false;
                } else if (it.kind == carview::RallyHubItem::kReturnColorado) {
                  rally_hub_open = false;
                  rally_world_switch = -1;
                  running = false;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_ESCAPE) rally_hub_open = false;
                else if (e.key.key == SDLK_UP || e.key.key == SDLK_W) rally_hub_selected = (rally_hub_selected + n_items - 1) % n_items;
                else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_S) rally_hub_selected = (rally_hub_selected + 1) % n_items;
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE) activate_rally();
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) rally_hub_open = false;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) rally_hub_selected = (rally_hub_selected + n_items - 1) % n_items;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) rally_hub_selected = (rally_hub_selected + 1) % n_items;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) activate_rally();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // The 1000 Club screen: up/down inspects challenges & hints, enter begins challenges in this car.
            if (club_open) {
              const size_t rows = (club_gate.empty() && club_car >= 0 && club_data.cars.count(club_car))
                                      ? std::max<size_t>(1, club_data.cars.at(club_car).size())
                                      : 1;
              auto begin = [&]() {
                if (!club_gate.empty() || club_car < 0 || !club_data.cars.count(club_car) || club_record()) return;
                drive::SavedClubCar rec;
                rec.car_id = club_car;
                for (const auto& ch : club_data.cars.at(club_car)) {
                  rec.challenges.emplace_back(ch.definition_hash, false);
                  rec.progress.emplace_back();
                }
                saved.club.push_back(std::move(rec));
                save_now("1000 club");
                club_check();
                notice = "1000 CLUB: CHALLENGES BEGUN IN THIS CAR";
                notice_until = vehicle.time() + 4;
                club_open = false;
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) club_open = false;
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) begin();
                else if (k == SDLK_UP || k == SDLK_W) club_pick = (club_pick + rows - 1) % rows;
                else if (k == SDLK_DOWN || k == SDLK_S) club_pick = (club_pick + 1) % rows;
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) club_open = false;
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) begin();
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) club_pick = (club_pick + rows - 1) % rows;
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) club_pick = (club_pick + 1) % rows;
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // Paint Shop & Vinyl/Livery Editor (LiveryEditor live preview + .liv persistence; legacy swatch fallback).
            if (paint_open && livery_editor.is_open()) {
              auto close_livery = [&](bool save_changes) {
                if (save_changes) {
                  std::string lerr;
                  const bool ok = livery_editor.save(livery_edit_path, &lerr);
                  sync_livery_preview(!livery_editor.document().layers.empty());
                  if (ok && have_garage) {
                    if (const garage::Car* sel = career_garage.selected_car()) {
                      if (garage::Car* gc = career_garage.find(sel->id);
                          gc && baked_ids.count(gc->car_id) && lower_ascii(baked_ids.at(gc->car_id)) == lower_ascii(car)) {
                        std::error_code ec;
                        const auto rel = !save_path.empty()
                                             ? std::filesystem::relative(livery_edit_path, save_path.parent_path(), ec)
                                             : std::filesystem::path{};
                        gc->livery_file = (!ec && !rel.empty()) ? rel.string() : livery_edit_path.string();
                        write_career_garage();
                      }
                    }
                  }
                  save_now("livery");
                  notice = ok ? "LIVERY SAVED & APPLIED" : ("LIVERY SAVE FAILED: " + lerr);
                  notice_until = vehicle.time() + 3.0;
                  audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kCancel);
                } else {
                  showroom.preview_colour(paint_before);
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                livery_editor.close();
                paint_open = false;
              };
              auto step_tab = [&](int dir) {
                livery_editor.move_tab(dir);
                audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto step_region = [&](int dir) {
                livery_editor.move_region(dir);
                audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto step_lr = [&](int dir) {
                if (livery_editor.tab() == carview::EditorTab::kPaint) {
                  if (livery_editor.region() == carview::PaintRegion::kWindowTint) {
                    livery_editor.step_window_tint(0.10f * float(dir));
                  } else {
                    livery_editor.step_hsv(float(dir) / 18.0f, 0.0f, 0.0f);
                  }
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                  livery_editor.move_special_color(dir);
                  livery_editor.apply_special_color();
                  sync_livery_preview(false);
                } else {
                  livery_editor.select_layer(dir);
                }
                audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto step_ud = [&](int dir) {
                if (livery_editor.tab() == carview::EditorTab::kPaint) {
                  if (livery_editor.region() == carview::PaintRegion::kWindowTint) {
                    livery_editor.step_window_tint(-0.10f * float(dir));
                  } else {
                    livery_editor.step_hsv(0.0f, 0.0f, -0.15f * float(dir));
                  }
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                  livery_editor.move_special_color(dir);
                  livery_editor.apply_special_color();
                  sync_livery_preview(false);
                } else {
                  livery_editor.cycle_layer_shape(dir);
                  sync_livery_preview(true);
                }
                audio_host.trigger_ui(audio::UiCue::kMove);
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) close_livery(false);
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) close_livery(true);
                else if (k == SDLK_Q) step_tab(-1);
                else if (k == SDLK_E || k == SDLK_TAB) step_tab(1);
                else if (k == SDLK_LEFTBRACKET) step_region(-1);
                else if (k == SDLK_RIGHTBRACKET) step_region(1);
                else if (k == SDLK_F && !e.key.repeat && livery_editor.tab() == carview::EditorTab::kPaint) {
                  livery_editor.move_finish(1);
                  sync_livery_preview(false);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_LEFT || k == SDLK_A) step_lr(-1);
                else if (k == SDLK_RIGHT || k == SDLK_D) step_lr(1);
                else if (k == SDLK_UP || k == SDLK_W) step_ud(-1);
                else if (k == SDLK_DOWN || k == SDLK_S) step_ud(1);
                else if (k == SDLK_Y && !e.key.repeat) {
                  if (livery_editor.tab() != carview::EditorTab::kVinylLayers)
                    livery_editor.set_tab(carview::EditorTab::kVinylLayers);
                  livery_editor.add_vinyl_layer();
                  sync_livery_preview(true);
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if ((k == SDLK_X || k == SDLK_DELETE) && !e.key.repeat) {
                  if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                    livery_editor.delete_selected_layer();
                    sync_livery_preview(true);
                  } else {
                    livery_editor.move_finish(1);
                    sync_livery_preview(false);
                  }
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                  if (k == SDLK_J) { livery_editor.transform_layer(-0.04f, 0, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_L) { livery_editor.transform_layer(0.04f, 0, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_I) { livery_editor.transform_layer(0, -0.04f, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_K) { livery_editor.transform_layer(0, 0.04f, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_R) { livery_editor.transform_layer(0, 0, 0, 0, 15.0f); sync_livery_preview(true); }
                  else if (k == SDLK_EQUALS || k == SDLK_PLUS) { livery_editor.transform_layer(0, 0, 0.05f, 0.05f, 0); sync_livery_preview(true); }
                  else if (k == SDLK_MINUS) { livery_editor.transform_layer(0, 0, -0.05f, -0.05f, 0); sync_livery_preview(true); }
                  else if (k == SDLK_P) { livery_editor.cycle_layer_plane(1); sync_livery_preview(true); }
                  else if (k == SDLK_H) { livery_editor.recolor_layer_hsv(1.0f / 12.0f, 0, 0); sync_livery_preview(true); }
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) close_livery(false);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) close_livery(true);
                else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) step_tab(-1);
                else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) step_tab(1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) step_lr(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) step_lr(1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) step_ud(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) step_ud(1);
                else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                  if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                    livery_editor.add_vinyl_layer();
                    sync_livery_preview(true);
                    audio_host.trigger_ui(audio::UiCue::kAccept);
                  } else {
                    step_region(1);
                  }
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                    livery_editor.delete_selected_layer();
                    sync_livery_preview(true);
                  } else {
                    livery_editor.move_finish(1);
                    sync_livery_preview(false);
                  }
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if (paint_open) {
              auto pick = [&](int by) {
                const int n = int(paint_order.size());
                paint_pick = size_t(((int(paint_pick) + by) % n + n) % n);
                showroom.preview_colour(paint_order[paint_pick]);
              };
              auto leave = [&](bool keep) {
                paint_open = false;
                const size_t chosen = paint_order[paint_pick];
                if (!keep || chosen == paint_before) {
                  showroom.preview_colour(paint_before);
                  return;
                }
                paint_sequence = showroom.colours()[chosen].sequence;
                paint_car = car;
                if (have_garage)
                  if (const garage::Car* sel = career_garage.selected_car())
                    if (garage::Car* gc = career_garage.find(sel->id);
                        gc && baked_ids.count(gc->car_id) && lower_ascii(baked_ids.at(gc->car_id)) == lower_ascii(car)) {
                      gc->colour = int(paint_sequence);
                      write_career_garage();
                    }
                save_now("paint");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                spawn[0] = at.x, spawn[1] = at.y, spawn[2] = at.z, spawn[3] = std::atan2(f.x, f.z);
                have_spawn = true;
                next_car = car;
                running = false;
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) leave(false);
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) leave(true);
                else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) pick(-1);
                else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) pick(1);
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) leave(false);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) leave(true);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) pick(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) pick(1);
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            slice12.on_sdl_event(e, pads);
            auto persist_slice12_settings = [&]() {
              const auto sfile = settings_path();
              if (sfile.empty()) return;
              std::vector<std::string> toks = read_settings(sfile, nullptr);
              auto fmt_num = [](double v) {
                char b[32];
                std::snprintf(b, sizeof b, "%.2f", v);
                return std::string(b);
              };
              const auto& ce = slice12.cam_editor_cfg;
              toks.push_back("--chase-height");
              toks.push_back(fmt_num(ce.chase_height_m));
              toks.push_back("--chase-distance");
              toks.push_back(fmt_num(ce.chase_distance_m));
              toks.push_back("--chase-fov");
              toks.push_back(fmt_num(ce.chase_fov_scale));
              toks.push_back("--chase-stiffness");
              toks.push_back(fmt_num(ce.chase_stiffness));
              toks.push_back("--driving-view");
              toks.push_back(ce.driving_view == drive::DrivingView::kWheel
                                 ? "wheel"
                                 : (ce.driving_view == drive::DrivingView::kReplay ? "replay" : "normal"));
              const auto& im = slice12.imm_cam_cfg;
              toks.push_back("--immersive-gforce");
              toks.push_back(fmt_num(im.gforce));
              toks.push_back("--immersive-lean");
              toks.push_back(fmt_num(im.lean));
              toks.push_back("--immersive-shake");
              toks.push_back(fmt_num(im.shake));
              toks.push_back("--immersive-head");
              toks.push_back(fmt_num(im.head));
              const auto& ht = slice12.ht_cfg;
              toks.push_back(ht.enabled ? "--head-tracking" : "--no-head-tracking");
              toks.push_back("--head-tracking-port");
              toks.push_back(std::to_string(ht.port));
              toks.push_back("--head-tracking-invert");
              toks.push_back(std::string(ht.invert[0] ? "1" : "0") + (ht.invert[1] ? "1" : "0") +
                             (ht.invert[2] ? "1" : "0"));
              if (slice12.trainer.config.saved_map_road.has_value()) {
                toks.push_back("--trainer-saved-road");
                toks.push_back(drive::format_road_point(*slice12.trainer.config.saved_map_road));
              }
              write_settings(sfile, toks, nullptr);
            };
            if (slice12.trainer_overlay.is_open()) {
              bool handled = false;
              const carview::Nav nv = carview::nav_from_sdl(e);
              const bool close_k = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F10);
              const bool tab_l = (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Q) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
              const bool tab_r = (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_E || e.key.key == SDLK_TAB)) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
              if (close_k || nv == carview::Nav::kBack) {
                slice12.trainer_overlay.close();
                audio_host.trigger_ui(audio::UiCue::kCancel);
                handled = true;
              } else if (tab_l) {
                slice12.trainer_overlay.switch_tab(-1);
                audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (tab_r) {
                slice12.trainer_overlay.switch_tab(1);
                audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kUp) {
                slice12.trainer_overlay.move(-1);
                audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kDown) {
                slice12.trainer_overlay.move(1);
                audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kLeft) {
                slice12.trainer_overlay.step(-1, slice12.trainer);
                audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kRight) {
                slice12.trainer_overlay.step(1, slice12.trainer);
                audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kAccept) {
                const auto res = slice12.trainer_overlay.activate(
                    slice12.trainer, career.credits, career.wristband_points, collected_wristband,
                    double(SDL_GetTicksNS()) * 1e-9, bool(race));
                if (res.credits_changed || res.wristband_changed) save_due = true;
                if (res.saved_road_changed) persist_slice12_settings();
                if (!res.message.empty()) notice = res.message, notice_until = vehicle.time() + 3.0;
                if (res.close_overlay) slice12.trainer_overlay.close();
                audio_host.trigger_ui(audio::UiCue::kAccept);
                handled = true;
              }
              if (handled) {
                // Keep FFB trace / telemetry probe synced live when toggled in Trainer Debug tab.
                const auto log_dir = settings_path().parent_path();
                if (slice12.trainer.config.debug_ffb_trace && !slice12.ffb_trace.is_recording())
                  slice12.ffb_trace.start(log_dir);
                else if (!slice12.trainer.config.debug_ffb_trace && slice12.ffb_trace.is_recording())
                  slice12.ffb_trace.stop();
                if (slice12.trainer.config.debug_telemetry_probe && !slice12.tel_probe.is_recording())
                  slice12.tel_probe.start(log_dir / "telemetry-probe.csv");
                else if (!slice12.trainer.config.debug_telemetry_probe && slice12.tel_probe.is_recording())
                  slice12.tel_probe.stop();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                  e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED)
                continue;
            }
            if (slice12.cam_editor_overlay.is_open()) {
              bool changed = false;
              const carview::Nav nv = carview::nav_from_sdl(e);
              const bool close_k = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F6);
              const bool tab_l = (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Q) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
              const bool tab_r = (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_E || e.key.key == SDLK_TAB)) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
              if (close_k || nv == carview::Nav::kBack) {
                slice12.cam_editor_overlay.close();
                persist_slice12_settings();
                audio_host.trigger_ui(audio::UiCue::kCancel);
              } else if (tab_l) {
                slice12.cam_editor_overlay.switch_tab(-1);
                audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (tab_r) {
                slice12.cam_editor_overlay.switch_tab(1);
                audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (nv == carview::Nav::kUp) {
                slice12.cam_editor_overlay.move(-1);
                audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (nv == carview::Nav::kDown) {
                slice12.cam_editor_overlay.move(1);
                audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (nv == carview::Nav::kLeft) {
                slice12.cam_editor_overlay.step(-1);
                audio_host.trigger_ui(audio::UiCue::kMove);
                changed = true;
              } else if (nv == carview::Nav::kRight) {
                slice12.cam_editor_overlay.step(1);
                audio_host.trigger_ui(audio::UiCue::kMove);
                changed = true;
              } else if (nv == carview::Nav::kAccept) {
                slice12.cam_editor_overlay.activate();
                audio_host.trigger_ui(audio::UiCue::kAccept);
                changed = true;
              }
              if (changed) {
                slice12.cam_editor_cfg = slice12.cam_editor_overlay.editor_config();
                slice12.imm_cam_cfg = slice12.cam_editor_overlay.immersive_config();
                slice12.ht_cfg = slice12.cam_editor_overlay.head_tracking_config();
                rig.set_camera_editor(slice12.cam_editor_cfg);
                rig.set_immersive_camera(slice12.imm_cam_cfg);
                slice12.ht_receiver.configure(slice12.ht_cfg);
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                  e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED)
                continue;
            }
            // F6 > MODS (mods_page.h): the overlay owns the input while open.
            if (mods_page.is_open()) {
              const carview::Nav nv = carview::nav_from_sdl(e);
              if (nv != carview::Nav::kNone) {
                audio_host.trigger_ui(nv == carview::Nav::kBack ? audio::UiCue::kCancel : audio::UiCue::kMove);
                if (mods_page.nav(nv)) reload_mod_textures();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // Profile > Save Backups & Save Sync (save_sync_ui.h).
            if (save_page.is_open()) {
              if (save_page.osk_open() && e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE) {
                save_page.backspace();
              } else if (save_page.osk_open() && e.type == SDL_EVENT_TEXT_INPUT && e.text.text && e.text.text[0]) {
                for (const char* p = e.text.text; *p; ++p) save_page.type_char(*p);
              } else {
                const carview::Nav nv = carview::nav_from_sdl(e);
                if (nv != carview::Nav::kNone) {
                  audio_host.trigger_ui(nv == carview::Nav::kBack ? audio::UiCue::kCancel : audio::UiCue::kMove);
                  save_page.nav(nv, save_session);
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // The settings page: up and down pick a row, left and right
            // change it (applied live), enter rebinds a key or saves.
            if (settings_page.is_open()) {
              auto apply_live = [&]() {
                const LiveSettings ls = settings_page.live_settings();
                live_controls = ls.controls;
                rumble_enabled = ls.controls.rumble_enabled;
                rumble_scale = ls.controls.rumble_scale;
                if (!rumble_enabled || rumble_scale <= 0.0) {
                  for (SDL_Gamepad* pad : pads) SDL_RumbleGamepad(pad, 0, 0, 0);
                }
                if (ls.camera_mode >= 0 && have_rig) {
                  rig.set_mode(drive::CameraMode(std::clamp(ls.camera_mode, 0, drive::kCameraModes - 1)));
                }
                if (have_rig && ls.controls.camera_effects != loaded_camera_effects) {
                  const drive::CameraMode cur_mode = rig.mode();
                  std::string cam_note;
                  if (load_camera_rig(game_root_of(), bake_dir, info, vehicle, rig, cam_note,
                                      ls.controls.camera_effects)) {
                    rig.set_mode(cur_mode);
                    loaded_camera_effects = ls.controls.camera_effects;
                  }
                }
                if (ls.difficulty >= 0 && ls.difficulty <= 4) race_options.difficulty = race::Difficulty(ls.difficulty);
                race_options.ai_mode = race::AiMode(std::clamp(ls.ai_mode, 0, 2));
                race_options.rubberband = ls.rubberband;
                if (race) {
                  race->set_difficulty(race_options.difficulty);
                  race->set_ai_mode(race_options.ai_mode);
                }
                live_sound_enabled = ls.sound;
                live_radio_enabled = ls.radio;
                treasure_map_setting = ls.treasure_map;
                live_motion_blur = ls.motion_blur;
                live_dof = ls.dof;
                skip_intro_movies = ls.skip_intro_movies;
                start_in_single_player = ls.start_in_single_player;
                skip_vo_with_a = ls.skip_vo_with_a;
                if (!ls.language.empty() && ls.language != language) {
                  language = ls.language;
                  race_options.language = language;
                  std::string le;
                  if (car_names) car_names->load(game_root_of(), state_root, &le, language, shared_db);
                  if (fh1::CjkFont::language_needs_cjk(language)) {
                    auto cf = std::make_shared<fh1::CjkFont>();
                    if (cf->load_for_language(game_root_of(), language, nullptr)) carview::ui::set_cjk_font(cf);
                  } else {
                    carview::ui::set_cjk_font(nullptr);
                  }
                  ui_atlas_synced = false;
                }
                live_photo_through_walls = ls.photo_through_walls;
                live_photo_wider_limits = ls.photo_wider_limits;
                photo_mode.set_limits(
                    carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
                audio_host.apply_config(carview::AudioConfig::from_tokens(settings_page.tokens()), user_music_dir);
                apply_vehicle_assists(vehicle, live_controls.assists);
                rewind_buf.set_enabled(live_controls.assists.rewind);
                driving_line_mode =
                    static_cast<drive::DrivingLineMode>(std::clamp(live_controls.assists.driving_line, 0, 2));
                race_options.assists = to_difficulty_assists(live_controls.assists);
                if (race) race->set_assists(race_options.assists);
                tod_clock.speed_scale = ls.day_length_scale;
                if (ls.tod_preset_hour >= 0.0) {
                  tod_clock.set_hour(ls.tod_preset_hour);
                  if (traffic) traffic->set_hour(tod_clock.hour());
                  renderer.reset_auto_exposure();
                }
                slice12.apply_live_settings(ls, rig, settings_path().parent_path());
                slice12.sync_pad_capabilities(pads);
                if (ls.graphics != active_graphics) {
                  const bool lut_changed = ls.graphics.lut != active_graphics.lut;
                  const bool ahdr_changed = ls.graphics.auto_hdr != active_graphics.auto_hdr;
                  active_graphics = ls.graphics;
                  renderer.set_graphics_settings(active_graphics);
                  showroom.set_texture_cap(active_graphics.texture_cap);
                  frame_pacer.set_interval(carview::modern::fps_cap_interval(active_graphics.fps_cap, display_hz));
                  if (lut_changed) {
                    if (active_graphics.lut.empty()) {
                      user_lut_active = false;
                      last_lut_zone = -2;
                    } else {
                      std::string lut_err;
                      if (apply_user_cube_lut(active_graphics.lut, carview::settings_path().parent_path(), renderer,
                                              lut_err)) {
                        user_lut_active = true;
                      } else {
                        user_lut_active = false;
                        last_lut_zone = -2;
                        notice = lut_err;
                        notice_until = vehicle.time() + 3.0;
                      }
                    }
                  }
                  if (ahdr_changed && window) {
                    if (active_graphics.auto_hdr) {
                      const auto st_dir = carview::settings_path().parent_path();
                      if (!st_dir.empty()) carview::set_display_hdr_restore_file(st_dir / "hdr_restore.bin");
                      carview::turn_on_display_hdr();
                    } else {
                      carview::restore_display_hdr();
                    }
                  }
                }
              };
              auto confirm = [&]() {
                if (settings_page.on_rebind_row()) {
                  settings_page.begin_rebind();
                  return;
                }
                if (!settings_page.on_save_line()) return;
                apply_live();
                std::string why;
                const auto file = settings_path();
                if (file.empty()) why = "no user folder";
                else if (!write_settings(file, settings_page.tokens(), &why)) why = why.empty() ? "not written" : why;
                save_now("settings");
                notice = why.empty() ? "settings saved and applied" : "settings: " + why;
                notice_until = vehicle.time() + 4;
                if (why.empty()) settings_page.close();
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (settings_page.capturing_key()) {
                  if (!e.key.repeat) {
                    if (k == SDLK_ESCAPE) {
                      settings_page.cancel_rebind();
                    } else {
                      const std::string kn = sdl_key_name(k);
                      if (!kn.empty() && settings_page.commit_rebind(kn)) apply_live();
                    }
                  }
                } else {
                  if ((k == SDLK_ESCAPE || k == SDLK_F6) && !e.key.repeat) {
                    settings_page.close();
                    settings_live_overlay = false;
                    audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                    audio_host.trigger_ui(audio::UiCue::kAccept);
                    confirm();
                  } else if (k == SDLK_UP || k == SDLK_W) {
                    settings_page.move(-1);
                    audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_DOWN || k == SDLK_S) {
                    settings_page.move(1);
                    audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_Q) {
                    settings_page.switch_tab(-1);
                    audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_M && !e.key.repeat) {
                    mods_page.open(mods_session);  // F6 > MODS
                    audio_host.trigger_ui(audio::UiCue::kAccept);
                  } else if (k == SDLK_P && !e.key.repeat) {
                    save_page.open(save_session, save_session.sync_report().conflict
                                                     ? carview::SaveProfilePage::Tab::kSync
                                                     : carview::SaveProfilePage::Tab::kBackups);
                    audio_host.trigger_ui(audio::UiCue::kAccept);
                  } else if (k == SDLK_E || k == SDLK_TAB) {
                    settings_page.switch_tab((e.key.mod & SDL_KMOD_SHIFT) ? -1 : +1);
                    audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_LEFT || k == SDLK_A) {
                    settings_page.step(-1);
                    apply_live();
                    audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_RIGHT || k == SDLK_D) {
                    settings_page.step(1);
                    apply_live();
                    audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !settings_page.capturing_key()) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  settings_page.close();
                  settings_live_overlay = false;
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm();
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  settings_page.move(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  settings_page.move(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  settings_page.switch_tab(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  mods_page.open(mods_session);  // F6 > MODS
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                  save_page.open(save_session, save_session.sync_report().conflict
                                                   ? carview::SaveProfilePage::Tab::kSync
                                                   : carview::SaveProfilePage::Tab::kBackups);
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  settings_page.switch_tab(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                  settings_page.step(-1);
                  apply_live();
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                  settings_page.step(1);
                  apply_live();
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // The career page scrolls while open; it changes nothing.
            if (career_page.is_open()) {
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) career_page.close();
                if (k == SDLK_UP || k == SDLK_W) career_page.move(-1);
                if (k == SDLK_DOWN || k == SDLK_S) career_page.move(1);
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) career_page.close();
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) career_page.move(-1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) career_page.move(1);
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            // The pause menu takes the keys and buttons while open.
            if (menu.open) {
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) {
                  menu.open = false;
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                if (k == SDLK_UP || k == SDLK_W) {
                  menu.move(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (k == SDLK_DOWN || k == SDLK_S) {
                  menu.move(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && (k == SDLK_LEFT || k == SDLK_A)) {
                  cycle_difficulty(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && (k == SDLK_RIGHT || k == SDLK_D)) {
                  cycle_difficulty(+1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                  choose();
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  menu.open = false;
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  menu.move(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  menu.move(1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                  cycle_difficulty(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                  cycle_difficulty(+1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                  choose();
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if (achievements.screen_open()) {
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if ((k == SDLK_ESCAPE || k == SDLK_F7) && !e.key.repeat) {
                  achievements.close_screen();
                  achievements.save_state(achievements_path);
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_LEFT || k == SDLK_A) {
                  achievements.move_focus(-1, 0);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_RIGHT || k == SDLK_D) {
                  achievements.move_focus(+1, 0);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_UP || k == SDLK_W) {
                  achievements.move_focus(0, -1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_DOWN || k == SDLK_S) {
                  achievements.move_focus(0, +1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_Q) {
                  achievements.switch_tab(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_E || k == SDLK_TAB) {
                  achievements.switch_tab((e.key.mod & SDL_KMOD_SHIFT) ? -1 : +1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_R && !e.key.repeat) {
                  achievements.cycle_sort();
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if ((k == SDLK_T || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  achievements.toggle_track_focused();
                  achievements.save_state(achievements_path);
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (k == SDLK_F8 && !e.key.repeat) {
                  pending_shot = PendingShot::kF8;
                  renderer.request_capture(true);
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) {
                  achievements.close_screen();
                  achievements.save_state(achievements_path);
                  audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                  achievements.move_focus(-1, 0);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                  achievements.move_focus(+1, 0);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  achievements.move_focus(0, -1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  achievements.move_focus(0, +1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  achievements.switch_tab(-1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  achievements.switch_tab(+1);
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  achievements.cycle_sort();
                  audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  achievements.toggle_track_focused();
                  achievements.save_state(achievements_path);
                  audio_host.trigger_ui(audio::UiCue::kAccept);
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if (photo_mode.active()) {
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) photo_mode.exit();
                else if (k == SDLK_TAB && !e.key.repeat) photo_mode.toggle_effects();
                else if ((k == SDLK_H || k == SDLK_F2) && !e.key.repeat) photo_mode.toggle_ui_hidden();
                else if (k == SDLK_UP) photo_mode.move_row(-1);
                else if (k == SDLK_DOWN) photo_mode.move_row(+1);
                else if (k == SDLK_LEFT) photo_mode.adjust_row(-1);
                else if (k == SDLK_RIGHT) photo_mode.adjust_row(+1);
                else if (k == SDLK_R && !e.key.repeat) photo_mode.effects().reset();
                else if (k == SDLK_F && !e.key.repeat) {
                  const drive::Vec3 pat = vehicle.position();
                  photo_mode.autofocus({pat.x, pat.y + 0.65, pat.z});
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  int cw = 0, ch = 0;
                  SDL_GetWindowSizeInPixels(window, &cw, &ch);
                  if (cw > 0 && ch > 0) {
                    photo_mode.begin_capture(uint32_t(cw), uint32_t(ch), 4);
                    pending_shot = PendingShot::kPhotoMode;
                    renderer.request_capture(false);
                    if (!race && pr_stunt_session.active()) pr_stunt_photo_pressed = true;
                  }
                } else if (k == SDLK_F8 && !e.key.repeat) {
                  pending_shot = PendingShot::kF8;
                  renderer.request_capture(!photo_mode.ui_hidden());
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) photo_mode.exit();
                else if (b == SDL_GAMEPAD_BUTTON_NORTH) photo_mode.toggle_effects();
                else if (b == SDL_GAMEPAD_BUTTON_BACK) photo_mode.toggle_ui_hidden();
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) photo_mode.move_row(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) photo_mode.move_row(+1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) photo_mode.adjust_row(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) photo_mode.adjust_row(+1);
                else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  const drive::Vec3 pat = vehicle.position();
                  photo_mode.autofocus({pat.x, pat.y + 0.65, pat.z});
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  int cw = 0, ch = 0;
                  SDL_GetWindowSizeInPixels(window, &cw, &ch);
                  if (cw > 0 && ch > 0) {
                    photo_mode.begin_capture(uint32_t(cw), uint32_t(ch), 4);
                    pending_shot = PendingShot::kPhotoMode;
                    renderer.request_capture(false);
                    if (!race && pr_stunt_session.active()) pr_stunt_photo_pressed = true;
                  }
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if ((replay_viewer.active() || replay_viewer.browser_open()) && !photo_mode.active()) {
              if (replay_viewer.browser_open()) {
                if (e.type == SDL_EVENT_KEY_DOWN) {
                  const SDL_Keycode k = e.key.key;
                  if (k == SDLK_ESCAPE) replay_viewer.close_browser();
                  else if (k == SDLK_UP || k == SDLK_W) replay_viewer.browser_move(-1);
                  else if (k == SDLK_DOWN || k == SDLK_S) replay_viewer.browser_move(+1);
                  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                    std::string lerr;
                    if (!replay_viewer.browser_load_selected(&lerr) && !lerr.empty())
                      replay_viewer.set_notice("LOAD FAILED: " + lerr);
                  }
                }
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                  const auto b = e.gbutton.button;
                  if (b == SDL_GAMEPAD_BUTTON_EAST) replay_viewer.close_browser();
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) replay_viewer.browser_move(-1);
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) replay_viewer.browser_move(+1);
                  else if (b == SDL_GAMEPAD_BUTTON_SOUTH) replay_viewer.browser_load_selected();
                }
              } else {
                if (e.type == SDL_EVENT_KEY_DOWN) {
                  const SDL_Keycode k = e.key.key;
                  if (k == SDLK_ESCAPE) replay_viewer.close();
                  else if (k == SDLK_SPACE && !e.key.repeat) replay_viewer.toggle_pause();
                  else if (k == SDLK_LEFT) {
                    if (replay_viewer.paused()) replay_viewer.step_frame(-1);
                    else replay_viewer.scrub(-1.5);
                  } else if (k == SDLK_RIGHT) {
                    if (replay_viewer.paused()) replay_viewer.step_frame(+1);
                    else replay_viewer.scrub(+1.5);
                  } else if (k == SDLK_LEFTBRACKET) replay_viewer.step_speed(-1);
                  else if (k == SDLK_RIGHTBRACKET) replay_viewer.step_speed(+1);
                  else if (k == SDLK_M && !e.key.repeat) replay_viewer.set_slow_motion();
                  else if (k == SDLK_C && !e.key.repeat) replay_viewer.cycle_camera(+1);
                  else if (k == SDLK_V && !e.key.repeat) replay_viewer.cycle_focus_car(+1);
                  else if (k == SDLK_T && !e.key.repeat) replay_viewer.toggle_telemetry();
                  else if ((k == SDLK_H || k == SDLK_F2) && !e.key.repeat) replay_viewer.toggle_ui_hidden();
                  else if (k == SDLK_S && !e.key.repeat) {
                    std::filesystem::path saved_r;
                    std::string serr;
                    if (!replay_viewer.save_to_dir(replays_dir, saved_r, &serr))
                      replay_viewer.set_notice("SAVE FAILED: " + serr);
                  } else if (k == SDLK_L && !e.key.repeat) {
                    replay_viewer.open_browser(replays_dir);
                  } else if (k == SDLK_P && !e.key.repeat) {
                    replay_viewer.set_paused(true);
                    if (const auto* fc = replay_viewer.focused_car()) {
                      const drive::Vec3 pf = drive::rotate({fc->quat[1], fc->quat[2], fc->quat[3], fc->quat[0]}, {0, 0, 1});
                      photo_mode.enter({double(fc->pos[0]), double(fc->pos[1]), double(fc->pos[2])},
                                       std::atan2(pf.x, pf.z),
                                       carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
                    }
                  } else if (k == SDLK_F8 && !e.key.repeat) {
                    pending_shot = PendingShot::kF8;
                    renderer.request_capture(!replay_viewer.ui_hidden());
                  }
                }
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                  const auto b = e.gbutton.button;
                  if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) replay_viewer.close();
                  else if (b == SDL_GAMEPAD_BUTTON_SOUTH) replay_viewer.toggle_pause();
                  else if (b == SDL_GAMEPAD_BUTTON_WEST) replay_viewer.cycle_camera(+1);
                  else if (b == SDL_GAMEPAD_BUTTON_NORTH) replay_viewer.toggle_telemetry();
                  else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) replay_viewer.trigger_rewind();
                  else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) replay_viewer.trigger_fast_forward();
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) replay_viewer.scrub(-2.0);
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) replay_viewer.scrub(+2.0);
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) replay_viewer.set_slow_motion();
                  else if (b == SDL_GAMEPAD_BUTTON_BACK) replay_viewer.toggle_ui_hidden();
                }
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if (rewind_buf.active()) {
              auto restore_camera = [&](const drive::RewindCameraState& cs) {
                if (have_rig && cs.rig.has_value()) rig = *cs.rig;
                if (cs.has_view) last_cam_view = cs.view, have_cam_view = true;
                front = cs.front_view;
                rig_accel.reset();
              };
              auto do_resume = [&]() {
                drive::RewindCameraState cs;
                if (rewind_buf.resume(vehicle, &cs)) {
                  particle_sys.clear();
                  smashables.clear_debris();
                  skid_sys.break_all();
                  restore_camera(cs);
                  drawn_before = vehicle.pose();
                  accumulator = 0.0;
                }
              };
              auto do_cancel = [&]() {
                drive::RewindCameraState cs;
                if (rewind_buf.cancel(vehicle, &cs)) {
                  skid_sys.break_all();
                  restore_camera(cs);
                  drawn_before = vehicle.pose();
                  accumulator = 0.0;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (rewind_buf.is_paused_at_keyframe() &&
                    (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE))
                  do_resume();
                else if (rewind_buf.is_paused_at_keyframe() && (k == SDLK_BACKSPACE || k == SDLK_Y))
                  rewind_buf.rewind_further();
                else if (k == SDLK_ESCAPE || k == SDLK_B)
                  do_cancel();
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (rewind_buf.is_paused_at_keyframe() && b == SDL_GAMEPAD_BUTTON_SOUTH)
                  do_resume();
                else if (rewind_buf.is_paused_at_keyframe() &&
                         (b == SDL_GAMEPAD_BUTTON_NORTH ||
                          (rewind_buf.rewind_on_back() && b == SDL_GAMEPAD_BUTTON_BACK)))
                  rewind_buf.rewind_further();
                else if (b == SDL_GAMEPAD_BUTTON_EAST)
                  do_cancel();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            auto try_trigger_rewind = [&]() {
              drive::RewindCameraState cs;
              if (have_rig) cs.rig = rig;
              cs.view = last_cam_view;
              cs.has_view = have_cam_view;
              cs.front_view = front;
              const double rc = race ? race->session().clock() : vehicle.time();
              if (!rewind_buf.trigger(vehicle.time(), rc, vehicle, cs)) {
                if (!rewind_buf.enabled()) notice = "rewind is off", notice_until = vehicle.time() + 2;
              }
            };
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
              const bool on_banner = race && race->player_finished() && !picking && results_page == 0;
              if (e.key.key == SDLK_ESCAPE) {
                if (on_banner && results_table) results_table = false;  // B: back to the banner
                else if (picking) picking = false;
                else if (!race && pr_stunt_session.active()) {
                  pr_stunt_session.cancel();
                  gps_nav.clear();
                  notice = "PR STUNT CANCELLED";
                  notice_until = vehicle.time() + 2.5;
                } else open_menu();
              }
              if (!race && pr_stunt_session.active() && e.key.key == SDLK_P) {
                pr_stunt_photo_pressed = true;
              } else if (e.key.key == SDLK_P && !picking && !(race && race->player_finished())) {
                const drive::Vec3 pat = vehicle.position();
                const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                                 carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
              }
              if (on_banner && e.key.key == SDLK_BACKSPACE) results_table = false;
              else if (!on_banner && e.key.key == SDLK_BACKSPACE) try_trigger_rewind();
              if (on_banner && e.key.key == SDLK_X) results_table = true;  // X: RACE RESULTS
              if (on_banner && e.key.key == SDLK_V) open_replay_viewer(false);  // V: VIEW REPLAY
              const std::string kn = sdl_key_name(e.key.key);
              if (!race && live_controls.keys.matches(drive::ControlAction::kActivate, kn)) activate_world_trigger();
              if (live_controls.keys.matches(drive::ControlAction::kReset, kn)) put_back();
              if (!on_banner && live_controls.keys.matches(drive::ControlAction::kRewind, kn)) try_trigger_rewind();
              if (live_controls.keys.matches(drive::ControlAction::kShiftUp, kn)) pending_shift = +1;
              if (live_controls.keys.matches(drive::ControlAction::kShiftDown, kn)) pending_shift = -1;
              if (e.key.key == SDLK_L) {
                if (e.key.mod & SDL_KMOD_SHIFT) {
                  light_override = (light_override + 1) % 4;
                  const char* lnames[4] = {"lights: auto", "lights: low beam", "lights: full beam", "lights: fog"};
                  notice = lnames[light_override];
                  notice_until = vehicle.time() + 2;
                } else {
                  driving_line_mode =
                      static_cast<drive::DrivingLineMode>((static_cast<int>(driving_line_mode) + 1) % 3);
                  live_controls.assists.driving_line = static_cast<int>(driving_line_mode);
                  const char* names[3] = {"driving line: off", "driving line: braking only", "driving line: full"};
                  notice = names[static_cast<int>(driving_line_mode)];
                  notice_until = vehicle.time() + 2;
                }
              }
              if (live_controls.keys.matches(drive::ControlAction::kMap, kn)) open_map();
              if (live_controls.keys.matches(drive::ControlAction::kRadioPrev, kn)) radio_press(-1);
              if (live_controls.keys.matches(drive::ControlAction::kRadioNext, kn)) radio_press(+1);
              if (audio_host.handle_key(e.key.key, e.key.mod, notice)) notice_until = vehicle.time() + 3;
              // SwitchCamera (the rig); without the rig, the front view.
              if (live_controls.keys.matches(drive::ControlAction::kSwitchCamera, kn)) {
                if (have_rig) rig.cycle();
                else front = !front;
              }
              if (e.key.key == SDLK_LEFTBRACKET) browse(-1);
              if (e.key.key == SDLK_RIGHTBRACKET) browse(1);
              const bool results = race && race->player_finished() && !picking;
              if (results && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) race_continue();
              else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                if (picking) take_pick();
                else activate_world_trigger();
              }
              if (results && e.key.key == SDLK_Y) race_restart();
              else if (!results && e.key.key == SDLK_Y) try_trigger_rewind();
              if (e.key.key == SDLK_T) {
                const bool back = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                tod_clock.step_hours(back ? -1.0 : 1.0);
                if (traffic) traffic->set_hour(tod_clock.hour());
                const int tot_min = int(tod_clock.tod_sec / 60.0);
                char line_buf[64];
                std::snprintf(line_buf, sizeof line_buf, "TIME %02d:%02d", (tot_min / 60) % 24, tot_min % 60);
                notice = line_buf;
                notice_until = vehicle.time() + 3;
              }
              // F6: live tabbed FH1/Pinyon Shift Settings overlay (Shift+F6 opens live Camera Editor).
              if (e.key.key == SDLK_F6) {
                if (e.key.mod & SDL_KMOD_SHIFT) {
                  slice12.cam_editor_overlay.open(slice12.cam_editor_cfg, slice12.imm_cam_cfg, slice12.ht_cfg);
                } else {
                  settings_page.load(read_settings(settings_path(), nullptr));
                  settings_page.open();
                  settings_live_overlay = true;
                }
                audio_host.trigger_ui(audio::UiCue::kMenuOpen);
              }
              // F10: Pinyon Shift Trainer overlay (isolated user-modded save).
              if (e.key.key == SDLK_F10) {
                slice12.trainer_overlay.toggle();
                audio_host.trigger_ui(slice12.trainer_overlay.is_open() ? audio::UiCue::kMenuOpen : audio::UiCue::kCancel);
              }
              // F5: reload the mods' replacement textures (texture_mods.h).
              if (e.key.key == SDLK_F5 && !e.key.repeat) reload_mod_textures();
              // F7: Pinyon Shift-style Achievements screen.
              if (e.key.key == SDLK_F7) {
                eval_achievements(false);
                achievements.open_screen();
                audio_host.trigger_ui(audio::UiCue::kMenuOpen);
              }
              // F8: real Vulkan swapchain PNG capture to <user-data>/photos/.
              if (e.key.key == SDLK_F8) {
                pending_shot = PendingShot::kF8;
                renderer.request_capture(show_hud);
              }
              // F12: pose report (carview-pose-N.txt) + companion PNG in photos/.
              if (e.key.key == SDLK_F12) {
                static int shot = 0;
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                char name[64], line_buf[96];
                std::snprintf(name, sizeof name, "carview-pose-%d.txt", ++shot);
                if (FILE* pf = std::fopen(name, "w")) {
                  std::fprintf(pf, "car %s\nposition %.2f %.2f %.2f\nheading %.1f deg\nspeed %.1f km/h\n"
                               "time %.2f s\nhour %s\ncamera %s\nspawn %.2f %.2f %.2f %.4f\n",
                               car.c_str(), at.x, at.y, at.z, std::atan2(f.x, f.z) * 57.29578, vehicle.speed() * 3.6,
                               vehicle.time(), traffic ? std::to_string(traffic->hour()).c_str() : "12 (no traffic)",
                               have_rig ? drive::camera_mode_name(rig.mode()) : (front ? "front" : "chase"), at.x, at.y, at.z,
                               std::atan2(f.x, f.z));
                  std::fclose(pf);
                  std::snprintf(line_buf, sizeof line_buf, "pose + png saved: %s", name);
                  pending_f12_pose_name = name;
                  pending_shot = PendingShot::kF12;
                  renderer.request_capture(show_hud);
                } else {
                  std::snprintf(line_buf, sizeof line_buf, "cannot write %s", name);
                }
                notice = line_buf;
                notice_until = vehicle.time() + 3;
              }
              if (e.key.key == SDLK_H || e.key.key == SDLK_F2) {
                const bool h2h_prompt =
                    e.key.key == SDLK_H && festival_mgr && !race && festival_mgr->hud(vehicle.time()).prompt_visible;
                if (h2h_prompt) {
                  const drive::Vec3 fp = vehicle.position();
                  const drive::Vec3 ff = drive::rotate(vehicle.orientation(), {0, 0, 1});
                  if (festival_mgr->start_head_to_head({fp.x, fp.y, fp.z}, std::atan2(ff.x, ff.z), vehicle.time())) {
                    club_pending.encounter_started = true;
                    notice = "HEAD-TO-HEAD RACE STARTED!";
                    notice_until = vehicle.time() + 2.5;
                    minimap_at = -1;
                  }
                } else {
                  show_hud = !show_hud;
                  if (!show_hud) {
                    for (int layer : {0, 1, 3}) renderer.set_overlay({}, 0, 0, 0, 0, layer);
                    renderer.set_ui({});
                    mm_size = 0;
                  }
                  radio_popup_gpu = {};
                  minimap_at = -1;
                }
              }
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
              const drive::ControllerLayout pad_layout = drive::default_controller_layout(live_controls.layout_id);
              const drive::PadControl pc = pad_control_from_button(e.gbutton.button);
              const std::string_view act = pad_layout.action_for(pc);
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) open_menu();
              if (act == "Rewind" && !(race && race->player_finished()) && !picking) {
                try_trigger_rewind();
              } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                if (rewind_buf.rewind_on_back() && !(race && race->player_finished()) && !picking) try_trigger_rewind();
                else open_map();
              } else if (act == "Map") {
                open_map();
              }
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH && act != "Rewind" && !rewind_buf.rewind_on_back() &&
                  !(race && race->player_finished()) && !picking)
                try_trigger_rewind();
              if (act == "RadioLeft") radio_press(-1);
              if (act == "RadioRight") radio_press(+1);
              if (act == "ShiftUp") pending_shift = +1;
              if (act == "ShiftDown") pending_shift = -1;
              if (act == "SwitchCamera") {
                if (picking || !have_rig) browse(1);
                else rig.cycle();
              }
              if (picking && e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) browse(-1);
              if (picking && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER && act != "SwitchCamera") browse(1);
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST && !(race && race->player_finished())) {
                if (picking) take_pick();
                else activate_world_trigger();
              }
              if (race && race->player_finished() && !picking) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) race_continue();
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) race_restart();
                // X opens the RACE RESULTS table from the banner, B closes it.
                if (results_page == 0 && e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) results_table = true;
                if (results_page == 0 && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) open_replay_viewer(false);
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) results_table = false;
              }
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
              if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) {
                pads.push_back(pad);
                std::printf("gamepad: %s\n", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "(unnamed)");
                slice12.sync_pad_capabilities(pads);
                slice12.sync_wheel_devices();
              }
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
              for (size_t k = 0; k < pads.size(); ++k)
                if (SDL_GetGamepadID(pads[k]) == e.gdevice.which) {
                  SDL_CloseGamepad(pads[k]);
                  pads.erase(pads.begin() + long(k));
                  slice12.sync_pad_capabilities(pads);
                  slice12.sync_wheel_devices();
                  break;
                }
            if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED) {
              slice12.sync_wheel_devices();
            }
          }
          if (!running) break;
          // Keyboard (rebindable), then the first gamepad with any input
          // (using the active ControllerLayouts preset and inside/outside deadzones).
          const bool* keys = SDL_GetKeyboardState(nullptr);
          drive::Input in;
          in.steer = (action_key_down(keys, live_controls.keys, drive::ControlAction::kSteerRight) ? 1.0 : 0.0) -
                     (action_key_down(keys, live_controls.keys, drive::ControlAction::kSteerLeft) ? 1.0 : 0.0);
          in.throttle = action_key_down(keys, live_controls.keys, drive::ControlAction::kThrottle) ? 1.0 : 0.0;
          in.brake = action_key_down(keys, live_controls.keys, drive::ControlAction::kBrake) ? 1.0 : 0.0;
          in.handbrake = action_key_down(keys, live_controls.keys, drive::ControlAction::kHandbrake);
          in.clutch = action_key_down(keys, live_controls.keys, drive::ControlAction::kClutch) ? 1.0 : 0.0;
          // Look around: the right stick turns the chase camera up to half a
          // turn, LookBack key held looks back.
          camera_pull.look_target =
              action_key_down(keys, live_controls.keys, drive::ControlAction::kLookBack) ? 3.14159265358979 : 0.0;
          double rig_look = camera_pull.look_target;
          const drive::ControllerLayout pad_layout = drive::default_controller_layout(live_controls.layout_id);
          const auto& dz = live_controls.deadzones;
          const drive::PadControl gas_ctrl = pad_layout.control_for("Gas");
          const drive::PadControl brake_ctrl = pad_layout.control_for("Brake");
          const drive::PadControl hb_ctrl = pad_layout.control_for("Handbrake");
          const drive::PadControl clutch_ctrl = pad_layout.control_for("Clutch");
          bool pad_used = false;
          for (SDL_Gamepad* pad : pads) {
            auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
            const bool is_wheel_pad = drive::is_known_wheel_vendor(SDL_GetGamepadVendor(pad));
            const double raw_steer = axis(SDL_GAMEPAD_AXIS_LEFTX);
            const double steer =
                is_wheel_pad
                    ? drive::shape_wheel_steering(raw_steer, slice12.wheel_shaping)
                    : drive::apply_deadzone_signed(raw_steer, dz.steering_inside, dz.steering_outside);
            const double raw_throttle = pad_control_value(pad, gas_ctrl, dz.throttle_inside, dz.throttle_outside);
            const double raw_brake = pad_control_value(pad, brake_ctrl, dz.brake_inside, dz.brake_outside);
            const double throttle = is_wheel_pad ? drive::shape_wheel_pedal(raw_throttle, slice12.wheel_shaping) : raw_throttle;
            const double brake = is_wheel_pad ? drive::shape_wheel_pedal(raw_brake, slice12.wheel_shaping) : raw_brake;
            const bool handbrake = pad_control_value(pad, hb_ctrl, dz.handbrake_inside, dz.handbrake_outside) > 0.5;
            const double clutch = pad_control_value(pad, clutch_ctrl, dz.clutch_inside, dz.clutch_outside);
            const double look = drive::apply_deadzone_signed(axis(SDL_GAMEPAD_AXIS_RIGHTX), 0.20, 0.95);
            if (look != 0) camera_pull.look_target = -look * 3.14159265358979;
            // The rig's look (SPEC-CAMERAS, Button): the right stick down
            // looks back, left and right to the sides, its click straight back.
            const double lx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ly = axis(SDL_GAMEPAD_AXIS_RIGHTY);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK) || (ly > 0.5 && ly > std::fabs(lx)))
              rig_look = 3.14159265358979;
            else if (std::fabs(lx) > 0.5 && std::fabs(lx) >= std::fabs(ly))
              rig_look = (lx > 0 ? 0.5 : -0.5) * 3.14159265358979;
            if (steer != 0 || throttle > 0 || brake > 0 || handbrake || clutch > 0) {
              in.steer = steer;
              in.throttle = throttle;
              in.brake = brake;
              in.handbrake = handbrake;
              in.clutch = clutch;
              pad_used = true;
              touch_controls.notify_gamepad_input(vehicle.time());
              break;
            }
          }
          slice12.apply_wheel_and_gyro_input(in, pad_used);
          // Apply Android multi-touch controls & edge-triggered HUD actions.
          touch_controls.set_gamepad_connected(!pads.empty());
          touch_controls.update(vehicle.time());
          {
            const carview::TouchActions ta = touch_controls.consume_actions();
            if (ta.pause) open_menu();
            if (ta.map) open_map();
            if (ta.camera) {
              if (have_rig) rig.cycle();
              else front = !front;
            }
            if (ta.radio_next) radio_press(+1);
            if (ta.shift_up) pending_shift = +1;
            if (ta.shift_down) pending_shift = -1;
            if (ta.rewind) {
              drive::RewindCameraState cs;
              if (have_rig) cs.rig = rig;
              cs.view = last_cam_view;
              cs.has_view = have_cam_view;
              cs.front_view = front;
              const double rc = race ? race->session().clock() : vehicle.time();
              rewind_buf.trigger(vehicle.time(), rc, vehicle, cs);
            }
            const carview::TouchDriveOutput to = touch_controls.output();
            if (to.active) {
              in.steer = to.steer;
              in.throttle = std::max(in.throttle, to.throttle);
              in.brake = std::max(in.brake, to.brake);
              in.handbrake = in.handbrake || to.handbrake;
            }
          }
          // Auto-Brake (Braking == 0 / Assisted, SPEC-CAR-PHYSICS): query the
          // active racing line's lookahead target speed upstream of stepping.
          if (!cruise && live_controls.assists.braking == 0 && race && !grid_menu_open && !race->player_finished()) {
            if (dl_samples.empty()) {
              drive::build_driving_line(race->racing_line(), braking_table, race->player_progress(),
                                        std::max(0.0, vehicle.speed()), drive::DrivingLineMode::kBrakingOnly,
                                        dl_samples);
            }
            if (!dl_samples.empty()) {
              drive::apply_auto_brake(in, vehicle.speed(), dl_samples.front().v_target);
            }
          }
          rig.set_look(rig_look);
          const uint64_t now = SDL_GetTicksNS();
          const double frame_dt = std::min(0.25, double(now - last) * 1e-9);
          title_orbit_time += frame_dt;
          auto apply_story_action = [&](const carview::StoryAction& sa) {
            if (sa.play_fmv) {
              const auto mp = carview::FmvPlayer::resolve_movie(game_root_of(), sa.fmv_id);
              if (!mp.empty() && fmv_player.open(mp, language)) {
                audio_host.set_fmv_audio(
                    [](void* ctx, float* out, int fr) { return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, fr); },
                    &fmv_player);
              }
            }
            if (sa.enter_title_screen && !no_title) show_title = true;
            if (sa.swap_car && !sa.car_media.empty() && sa.car_media != car) {
              if (std::find(baked_cars.begin(), baked_cars.end(), sa.car_media) != baked_cars.end()) {
                next_car = sa.car_media;
                running = false;
              }
            }
            if (sa.teleport) {
              vehicle.reset(drive::Vec3{sa.pos[0], sa.pos[1], sa.pos[2]}, sa.heading);
            }
            if (sa.launch_qualify_race) {
              std::string why;
              if (start_race(std::to_string(sa.qualify_route_id), why)) {
                grid_menu_open = true;
                grid_menu_selected = 0;
              }
            }
            if (sa.grant_starter) {
              award_garage_car(race::kStarterCarId, race::kStarterCarColour, true);
              if (career.credits < 15000) career.credits = 15000;
            }
            if (!sa.radio_immediate_event.empty()) audio_host.radio_trigger_immediate(sa.radio_immediate_event);
            if (!sa.vo_trigger.empty()) audio_host.trigger_vo(sa.vo_trigger, false);
            if (sa.open_autoshow) open_shop_or_autoshow(true);
            if (sa.intro_finished) saved.intro_completed = true;
          };
          if (!boot_intro_checked && !headless) {
            boot_intro_checked = true;
            if (!cli_fmv_path.empty()) {
              if (fmv_player.open(cli_fmv_path, language)) {
                audio_host.set_fmv_audio(
                    [](void* ctx, float* out, int fr) { return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, fr); },
                    &fmv_player);
              }
            } else if (story_intro_cli) {
              show_title = false;
              apply_story_action(story_intro.begin_new_career(game_root_of(), skip_intro_movies));
            } else if (!skip_intro_movies && show_title) {
              show_title = false;
              apply_story_action(story_intro.begin_boot(game_root_of(), skip_intro_movies, start_in_single_player,
                                                        have_saved && !saved.intro_completed));
            }
          }
          if (fmv_player.active()) {
            if (!fmv_player.update(frame_dt, true)) audio_host.set_fmv_audio(nullptr, nullptr);
          }
          if (story_intro.active()) {
            const drive::Vec3 vp = vehicle.position();
            const bool rf = race && race->player_finished();
            apply_story_action(story_intro.update(frame_dt, {vp.x, vp.z}, fmv_player.active(), rf));
          }
          {
            const drive::Vec3 vp = vehicle.position();
            const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            audio_host.update_world_acoustics(float(vp.x), float(vp.y), float(vp.z), std::atan2(float(vf.x), float(vf.z)));
            audio_host.radio_set_time_of_day(tod_clock.hour());
          }
          const bool trainer_freecam_active = slice12.trainer.effective_free_camera();
          const bool menu_paused = show_title || fmv_player.active() || ceremony.active || grid_menu_open || menu.open ||
                                   map_open || events_page.is_open() || outpost_hub_open || garage_open ||
                                   shop.is_open() || autoshow.is_open() || career_page.is_open() ||
                                   settings_page.is_open() || paint_open || club_open || rally_hub_open ||
                                   rally_service_open || rally_results_open || photo_mode.active() ||
                                   replay_viewer.active() || replay_viewer.browser_open() || achievements.screen_open() ||
                                   mods_page.is_open() || save_page.is_open() || slice12.trainer_overlay.is_open() ||
                                   trainer_freecam_active;
          const bool sim_paused = menu_paused || rewind_buf.active();
          // Poll queued F10 Trainer GPS & Teleport requests (last/saved map road).
          {
            const double now_wall_s = double(now) * 1e-9;
            if (const auto gps_pt = slice12.trainer.consume_gps(race != nullptr);
                gps_pt.has_value() && gps_solver.ready()) {
              const drive::Vec3 at = vehicle.position();
              fire_satnav_event(gps_nav.set_destination((*gps_pt)[0], (*gps_pt)[2], traffic::GpsDestKind::kGeneric,
                                                        "TRAINER ROAD", at.x, at.z));
              notice = "GPS ROUTED TO TRAINER ROAD";
              notice_until = vehicle.time() + 3.0;
            }
            if (const auto tp_pt = slice12.trainer.consume_teleport(now_wall_s, true, race != nullptr);
                tp_pt.has_value()) {
              spawn[0] = (*tp_pt)[0];
              spawn[1] = (*tp_pt)[1];
              spawn[2] = (*tp_pt)[2];
              spawn[3] = 0.0;
              have_spawn = true;
              fast_travel = true;
              next_car = car;
              running = false;
            }
          }
          achievements.update(frame_dt);
          save_page.update(frame_dt);
          if (!save_session.notices.empty()) {
            notice = save_session.notices.front().first;
            notice_until = vehicle.time() + save_session.notices.front().second;
            save_session.notices.erase(save_session.notices.begin());
          }
          // Native mods' per-frame hook (shift_mod_api.h).
          if (mods_session.plugins().alive()) {
            static uint32_t mod_frame = 0;
            const drive::Vec3 mp = vehicle.position();
            const drive::Vec3 mf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            ShiftModFrame frame{};
            frame.size = sizeof frame;
            frame.frame = mod_frame++;
            frame.time = vehicle.time();
            frame.dt = frame_dt;
            frame.position[0] = float(mp.x), frame.position[1] = float(mp.y), frame.position[2] = float(mp.z);
            frame.heading = std::atan2(float(mf.x), float(mf.z));
            frame.speed = float(vehicle.speed());
            frame.rpm = float(vehicle.rpm());
            frame.gear = vehicle.gear();
            frame.paused = menu_paused ? 1 : 0;
            mods_session.tick(frame);
            for (const auto& [text, seconds] : mods_session.notices) notice = text, notice_until = vehicle.time() + seconds;
            mods_session.notices.clear();
          }
          if (sim_paused != was_sim_paused) {
            rig_accel.reset();
            was_sim_paused = sim_paused;
          }
          if (!menu_paused) rewind_buf.update(frame_dt);
          if (rewind_buf.consume_view_replay_request()) {
            rewind_buf.cancel(vehicle);
            open_replay_viewer(false);
          }
          if (replay_viewer.active() && !photo_mode.active()) {
            replay_viewer.update(frame_dt);
          }
          if (photo_mode.active()) {
            carview::PhotoCameraInput pcin{};
            const bool* kb = SDL_GetKeyboardState(nullptr);
            if (kb[SDL_SCANCODE_W]) pcin.move_forward += 1.0f;
            if (kb[SDL_SCANCODE_S]) pcin.move_forward -= 1.0f;
            if (kb[SDL_SCANCODE_D]) pcin.move_right += 1.0f;
            if (kb[SDL_SCANCODE_A]) pcin.move_right -= 1.0f;
            if (kb[SDL_SCANCODE_E]) pcin.move_up += 1.0f;
            if (kb[SDL_SCANCODE_Q]) pcin.move_up -= 1.0f;
            if (kb[SDL_SCANCODE_L]) pcin.look_yaw += 1.0f;
            if (kb[SDL_SCANCODE_J]) pcin.look_yaw -= 1.0f;
            if (kb[SDL_SCANCODE_I]) pcin.look_pitch += 1.0f;
            if (kb[SDL_SCANCODE_K]) pcin.look_pitch -= 1.0f;
            if (kb[SDL_SCANCODE_Z]) pcin.zoom += 1.0f;
            if (kb[SDL_SCANCODE_X]) pcin.zoom -= 1.0f;
            if (kb[SDL_SCANCODE_COMMA]) pcin.roll -= 1.0f;
            if (kb[SDL_SCANCODE_PERIOD]) pcin.roll += 1.0f;
            for (SDL_Gamepad* pad : pads) {
              pcin.move_right += float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, 0.16, 0.95));
              pcin.move_forward -= float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0, 0.16, 0.95));
              pcin.look_yaw += float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0, 0.16, 0.95));
              pcin.look_pitch -= float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0, 0.16, 0.95));
              pcin.move_up += float(std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0) -
                                    std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0));
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pcin.zoom += 1.0f;
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pcin.zoom -= 1.0f;
            }
            auto gb = [&](double x, double y, double z, double& gy) {
              drive::Hit hit;
              if (!ground || !ground->raycast({x, y, z}, {0, -1, 0}, 80.0, hit)) return false;
              gy = y - hit.distance;
              return true;
            };
            photo_mode.step(frame_dt, pcin, gb);
          }
          // Trainer Free Camera fly controls (active when F10 Trainer > World > Free Camera is ON).
          static bool trainer_freecam_was_active = false;
          if (trainer_freecam_active && !slice12.trainer_overlay.is_open() && !photo_mode.active()) {
            if (!trainer_freecam_was_active) {
              if (have_cam_view) {
                slice12.trainer.free_cam.reset_from_pose(last_cam_view.eye, last_cam_view.forward);
              } else {
                const drive::Vec3 vp = vehicle.position();
                const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                slice12.trainer.free_cam.reset_from_pose({vp.x - vf.x * 5.0, vp.y + 2.5, vp.z - vf.z * 5.0},
                                                         {vf.x, 0.0, vf.z});
              }
            }
            const bool* kb = SDL_GetKeyboardState(nullptr);
            double mf = 0, mr = 0, mu = 0, ty = 0, tp = 0;
            if (kb[SDL_SCANCODE_W] || kb[SDL_SCANCODE_UP]) mf += 1.0;
            if (kb[SDL_SCANCODE_S] || kb[SDL_SCANCODE_DOWN]) mf -= 1.0;
            if (kb[SDL_SCANCODE_D] || kb[SDL_SCANCODE_RIGHT]) mr += 1.0;
            if (kb[SDL_SCANCODE_A] || kb[SDL_SCANCODE_LEFT]) mr -= 1.0;
            if (kb[SDL_SCANCODE_E] || kb[SDL_SCANCODE_SPACE]) mu += 1.0;
            if (kb[SDL_SCANCODE_Q] || kb[SDL_SCANCODE_LCTRL]) mu -= 1.0;
            if (kb[SDL_SCANCODE_L]) ty += 1.0;
            if (kb[SDL_SCANCODE_J]) ty -= 1.0;
            if (kb[SDL_SCANCODE_I]) tp += 1.0;
            if (kb[SDL_SCANCODE_K]) tp -= 1.0;
            bool fast_fly = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
            for (SDL_Gamepad* pad : pads) {
              mr += drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, 0.16, 0.95);
              mf -= drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0, 0.16, 0.95);
              ty += drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0, 0.16, 0.95);
              tp -= drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0, 0.16, 0.95);
              mu += std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0) -
                    std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0);
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) fast_fly = true;
            }
            slice12.trainer.free_cam.step(frame_dt, mf, mr, mu, ty, tp, fast_fly);
          }
          trainer_freecam_was_active = trainer_freecam_active;
          // Paused: the car holds still (no steps, no time to catch up).
          const double game_spd = slice12.trainer.effective_game_speed();
          tod_frame_dt = frame_dt * game_spd;
          {
            const double fixed_h = slice12.trainer.effective_fixed_hour();
            if (fixed_h >= 0.0) {
              tod_clock.set_hour(fixed_h);
              if (traffic) traffic->set_hour(tod_clock.hour());
            } else if (!sim_paused) {
              if (tod_system.loaded_tod && !race) {
                const double spd = tod_system.active_preset(tod_clock).scalar("TimeSpeed", float(tod_clock.tod_sec / 60.0), 28.0f);
                const double prev_scale = tod_clock.speed_scale;
                if (slice12.trainer.config.cheats_enabled && slice12.trainer.config.day_length_minutes > 0.0) {
                  tod_clock.speed_scale = slice12.trainer.effective_day_length_scale();
                }
                tod_clock.advance(frame_dt * game_spd, spd);
                tod_clock.speed_scale = prev_scale;
                if (traffic) traffic->set_hour(tod_clock.hour());
              }
            }
          }
          if (!sim_paused) {
            accumulator += frame_dt * game_spd;
          }
          last = now;
          if (vehicle.time() >= next_save) {
            save_now("timer");
            next_save = vehicle.time() + 30;
          }
          if (save_due && !map_open) {  // a collectible or award: micro-save
            save_due = false;
            save_now("collectible");
            eval_achievements(true);
          }
          if (!sim_paused && vehicle.time() >= next_achievement_eval) {
            eval_achievements(true);
            next_achievement_eval = vehicle.time() + 1.0;
          }
          double frame_world_impact = 0.0;
          while (!sim_paused && accumulator >= drive::Vehicle::kStep) {
            drawn_before = vehicle.pose();
            drive::Input step_in = cruise ? follower.input(vehicle) : in;
            if (!cruise) {
              step_in.shift = pending_shift;
              pending_shift = 0;
            }
            if (!step(step_in)) {
              running = false;
              status = 1;
              std::fprintf(stderr, "carview: %s\n", error.c_str());
              break;
            }
            if (vehicle.hand_of_god_triggered()) {
              vehicle.clear_hand_of_god();
              put_back();
            }
            {
              drive::RewindCameraState cs;
              if (have_rig) cs.rig = rig;
              cs.view = last_cam_view;
              cs.has_view = have_cam_view;
              cs.front_view = front;
              const double rc = race ? race->session().clock() : vehicle.time();
              const bool cap_ok = !race || (race->session().clock() >= 0.0 && !race->player_finished());
              rewind_buf.record_step(vehicle.time(), rc, vehicle, cs, cap_ok);
              // 60 Hz gameplay replay ring-buffer recording (player + opponents + nearby traffic).
              if (replay_rec.active()) {
                uint8_t pl_place = 0;
                if (race) {
                  const auto rh = race->hud_info();
                  pl_place = uint8_t(std::clamp(rh.place, 0, 255));
                  if (rh.lap > replay_last_lap) {
                    replay_last_lap = rh.lap;
                    replay_rec.add_marker(vehicle.time(), "LAP " + std::to_string(rh.lap));
                  }
                  if (race->player_finished() && !replay_finish_marked) {
                    replay_finish_marked = true;
                    replay_rec.add_marker(vehicle.time(), "FINISH");
                  }
                }
                std::vector<drive::ReplayCarSample> rcars;
                rcars.push_back(drive::ReplayCarSample::from_vehicle(
                    0u, vehicle, player_lamps, in.throttle, in.brake, in.handbrake ? 1.0 : 0.0, in.steer, pl_place));
                if (race) {
                  std::vector<carview::RaceView::OpponentTag> otags;
                  race->opponent_tags(otags);
                  uint32_t slot = 1;
                  for (const auto& ot : otags) {
                    drive::ReplayCarSample os{};
                    os.slot = slot++;
                    os.pos[0] = float(ot.x), os.pos[1] = float(ot.y), os.pos[2] = float(ot.z);
                    os.vel[0] = float(ot.vx), os.vel[1] = float(ot.vy), os.vel[2] = float(ot.vz);
                    os.speed_mps = float(ot.speed);
                    os.rpm = float(ot.rpm);
                    const double hdg = std::atan2(ot.vx, ot.vz);
                    os.quat[0] = float(std::cos(0.5 * hdg));
                    os.quat[2] = float(std::sin(0.5 * hdg));
                    rcars.push_back(os);
                  }
                }
                if (traffic && traffic->cars()) {
                  const drive::Vec3 pat = vehicle.position();
                  uint32_t tslot = 100;
                  for (const auto& tc : *traffic->cars()) {
                    const double dx = tc.position.x - pat.x, dz = tc.position.z - pat.z;
                    if (dx * dx + dz * dz > 160.0 * 160.0) continue;
                    drive::ReplayCarSample ts{};
                    ts.slot = tslot++;
                    ts.pos[0] = float(tc.position.x), ts.pos[1] = float(tc.position.y), ts.pos[2] = float(tc.position.z);
                    ts.speed_mps = float(tc.speed);
                    ts.quat[0] = float(std::cos(0.5 * tc.heading));
                    ts.quat[2] = float(std::sin(0.5 * tc.heading));
                    rcars.push_back(ts);
                    if (tslot >= 116) break;
                  }
                }
                replay_rec.maybe_record(vehicle.time(), std::move(rcars));
              }
            }
            frame_world_impact = std::max(frame_world_impact, vehicle.body_contact_force());
            {
              const double mass = std::max(100.0, vehicle.params().mass);
              const auto& bc = vehicle.strongest_body_contact();
              const double dv_world = bc.force * drive::Vehicle::kStep / mass;
              const double dv_car = traffic ? traffic->player_impulse() / mass : 0.0;
              if (dv_world > 1.0) {
                audio::CollisionHit hit{};
                hit.rel_speed_mps = float(dv_world);
                hit.pos = {float(bc.point.x), float(bc.point.y), float(bc.point.z)};
                audio_host.trigger_collision(hit);
                const drive::Vec3 rel = bc.point - vehicle.position();
                const drive::Quat q = vehicle.orientation();
                const drive::Quat qinv{-q.x, -q.y, -q.z, q.w};
                const drive::Vec3 loc = drive::rotate(qinv, rel);
                car_damage.record_impact(float(loc.x), float(loc.y), float(loc.z), float(dv_world));
              } else if (dv_car > 1.0) {
                const drive::Vec3 vp = vehicle.position();
                audio::CollisionHit hit{};
                hit.rel_speed_mps = float(dv_car);
                hit.pos = {float(vp.x), float(vp.y), float(vp.z)};
                hit.is_car_on_car = true;
                audio_host.trigger_collision(hit);
                car_damage.record_impact(0.0f, 0.5f, 1.5f, float(dv_car));
              }
              const auto& ws = vehicle.wheels();
              const bool offroad = (ws[0].surface >= 2 || ws[2].surface >= 2);
              const std::array<double, 4> slips = {ws[0].slip_ratio, ws[1].slip_ratio, ws[2].slip_ratio, ws[3].slip_ratio};
              const double rpm_n = vehicle.rpm() / std::max(1000.0, vehicle.params().redline_rpm);
              car_damage.step(drive::Vehicle::kStep, vehicle.speed(), offroad, step_in.throttle, rpm_n, slips);
              showroom.set_damage_state(car_damage);
            }
            accumulator -= drive::Vehicle::kStep;
          }
          double rumble_low_f = 0.0, rumble_high_f = 0.0;
          if (!pads.empty() && gamepad_rumble.config().loaded) {
            drive::RumbleInput ri;
            const auto& vp = vehicle.params();
            const double rpm_span = std::max(1.0, vp.redline_rpm - vp.idle_rpm);
            ri.engine_norm_rpm = std::clamp((vehicle.rpm() - vp.idle_rpm) / rpm_span, 0.0, 1.0);
            const auto& ws = vehicle.wheels();
            double fl_load = 0, fl_slip = 0, rl_load = 0, rl_slip = 0;
            for (int wi = 0; wi < 4; ++wi) {
              const double load = std::max(0.0, ws[size_t(wi)].load);
              const double slip = std::hypot(ws[size_t(wi)].slip_ratio, ws[size_t(wi)].slip_angle);
              if (wi < 2) fl_load += load, fl_slip += load * slip;
              else rl_load += load, rl_slip += load * slip;
            }
            ri.tire_traction_front = fl_load > 1e-3 ? fl_slip / fl_load : 0.0;
            ri.tire_traction_rear = rl_load > 1e-3 ? rl_slip / rl_load : 0.0;
            ri.tire_traction = (fl_load + rl_load) > 1e-3 ? (fl_slip + rl_slip) / (fl_load + rl_load) : 0.0;
            // Surface bumpiness: use exact 2 * Bumpiness.Rumble from Vehicle
            // when available, else fallback to off-roadness estimate, scaled by speed.
            double bump_2x = vehicle.surface_bump_rumble();
            if (bump_2x <= 0.0) {
              double max_bump = 0.0;
              const auto& surfs = collision.surfaces();
              for (int wi = 0; wi < 4; ++wi) {
                if (!ws[size_t(wi)].contact) continue;
                const int si = ws[size_t(wi)].surface;
                double bump = 0.02;
                if (si >= 0 && size_t(si) < surfs.size()) {
                  const auto& sf = surfs[size_t(si)];
                  if (sf.off_roadness > 0.0f) bump = 0.02 + 0.38 * sf.off_roadness;
                  else if (sf.friction_scale < 0.95f) bump = 0.15;
                }
                max_bump = std::max(max_bump, bump);
              }
              bump_2x = 2.0 * max_bump;
            }
            ri.surface_bumpiness = bump_2x * (std::fabs(vehicle.speed()) / 22.352);
            ri.car_to_world_impulse = frame_world_impact / 500.0;
            ri.on_shift = vehicle.gear() != prev_rumble_gear;
            prev_rumble_gear = vehicle.gear();
            ri.paused = sim_paused;
            ri.enabled = rumble_enabled;
            ri.scale = rumble_scale;
            const auto ro = gamepad_rumble.update(frame_dt, ri);
            for (SDL_Gamepad* pad : pads) SDL_RumbleGamepad(pad, ro.left_u16(), ro.right_u16(), 100);
            rumble_low_f = ro.left_u16() / 65535.0;
            rumble_high_f = ro.right_u16() / 65535.0;
          }
          slice12.tick(vehicle, rig, pads, !sim_paused && !show_title, frame_dt, now,
                       uint32_t(now / 1000000ULL), rumble_low_f, rumble_high_f);
          if (now - title_at > 100000000) {  // the speedometer, ten times a second
            title_at = now;
            char title[320], gear[8];
            if (vehicle.gear() < 0) std::snprintf(gear, sizeof gear, "R");
            else if (vehicle.gear() == 0) std::snprintf(gear, sizeof gear, "N");
            else std::snprintf(gear, sizeof gear, "%d", vehicle.gear());
            {
              const drive::Vec3 at = vehicle.position();
              const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
              std::snprintf(title, sizeof title,
                            "carview drive - %s - %3.0f km/h  gear %s  %5.0f rpm%s   at %.0f %.0f %.0f heading %.0f",
                            car.c_str(), std::fabs(vehicle.speed()) * 3.6, gear, vehicle.rpm(),
                            vehicle.abs_active() ? "  ABS" : "", at.x, at.y, at.z, std::atan2(f.x, f.z) * 57.29578);
            }
            if (picking && pick < baked_cars.size()) {
              const size_t n = std::strlen(title);
              std::snprintf(title + n, sizeof title - n, "   |   pick %zu/%zu: %s (Enter or X to drive it)", pick + 1,
                            baked_cars.size(), baked_cars[pick].c_str());
            }
            SDL_SetWindowTitle(window, title);
          }
          const audio::View cur_audio_view = audio_view();
          audio_host.update_frame(sound_inputs(cur_audio_view),
                                  menu_paused || rewind_buf.is_rewinding() || rewind_buf.is_paused_at_keyframe(),
                                  menu.open || settings_page.is_open() || map_open, show_title, false);
          {
            audio::VehicleAudioState vst{};
            const auto& vp = vehicle.params();
            const drive::Vec3 vpos = vehicle.position();
            const drive::Vec3 vvel = vehicle.velocity();
            const drive::Vec3 vright = drive::rotate(vehicle.orientation(), {1, 0, 0});
            vst.speed_mph = float(std::fabs(vehicle.speed()) * 2.2369363);
            vst.rpm = float(vehicle.rpm());
            vst.idle_rpm = float(vp.idle_rpm);
            vst.redline_rpm = float(vp.redline_rpm);
            vst.throttle = float(vehicle.engine_throttle());
            vst.gear = vehicle.gear();
            vst.shifting = vehicle.shifting();
            vst.clutch_pos = float(1.0 - vehicle.clutch());
            vst.forced_induction = vp.has_turbo ? 1 : (vp.has_supercharger ? 2 : 0);
            vst.turbo_psi = float(vehicle.boost_psi());
            vst.turbo_norm_rpm = vp.has_turbo && vp.boost_max_psi > 1.0
                                     ? float(std::clamp(vehicle.boost_psi() / vp.boost_max_psi, 0.0, 1.2))
                                     : 0.0f;
            vst.bov_open = vehicle.bov_open();
            vst.supercharger_norm_rpm = vp.has_supercharger && vp.redline_rpm > 1.0
                                            ? float(std::clamp(vehicle.rpm() / vp.redline_rpm, 0.0, 1.2))
                                            : 0.0f;
            vst.trans_level = audio_trans_level;
            vst.turbo_level = audio_turbo_level;
            vst.sc_level = audio_sc_level;
            vst.backfire_level = std::max(audio_upgrades.exhaust, vp.has_turbo ? 1 : 0);
            const auto& ws = vehicle.wheels();
            const auto& surfs = collision.surfaces();
            for (int wi = 0; wi < 4; ++wi) {
              auto& wa = vst.wheels[size_t(wi)];
              const auto& wstate = ws[size_t(wi)];
              wa.slip_combined = float(std::hypot(wstate.slip_ratio, wstate.slip_angle));
              wa.wheel_rpm = float(wstate.omega * 9.5492966);
              const double sload = std::max(100.0, vehicle.static_load(wi));
              wa.load = wstate.contact ? float(std::clamp(wstate.load / sload, 0.0, 2.0)) : 0.0f;
              const int axle = wi < 2 ? 0 : 1;
              if (wstate.contact && wstate.compression > vp.bump_travel[size_t(axle)] * 0.92)
                wa.bottom_out_force = float(wstate.compression - vp.bump_travel[size_t(axle)] * 0.92);
              uint8_t sgrp = 0;
              if (wstate.surface >= 0 && size_t(wstate.surface) < surfs.size()) {
                const auto& sf = surfs[size_t(wstate.surface)];
                if (sf.off_roadness > 0.5f) sgrp = 2;
                else if (sf.off_roadness > 0.05f) sgrp = 1;
              }
              wa.surface_group = sgrp;
            }
            vst.listener_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
            vst.listener_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
            vst.listener_right = {float(vright.x), float(vright.y), float(vright.z)};
            vst.cockpit_view = cur_audio_view == audio::View::kCockpit;
            vst.in_race = race != nullptr;
            vst.rewinding = rewind_buf.is_rewinding();

            // Gather up to kMaxOtherCars nearest race opponents and traffic cars for 3D audio.
            struct Cand {
              double d2 = 0;
              audio::OtherCarAudio car{};
            };
            std::vector<Cand> cands;
            if (race) {
              std::vector<carview::RaceView::OpponentTag> otags;
              race->opponent_tags(otags);
              for (const auto& ot : otags) {
                const double dx = ot.x - vpos.x, dy = ot.y - vpos.y, dz = ot.z - vpos.z;
                const double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > 220.0 * 220.0) continue;
                audio::OtherCarAudio oc{};
                oc.active = true;
                oc.is_traffic = false;
                oc.pos = {float(ot.x), float(ot.y), float(ot.z)};
                oc.vel = {float(ot.vx), float(ot.vy), float(ot.vz)};
                oc.rpm = float(ot.rpm);
                oc.speed_mph = float(std::fabs(ot.speed) * 2.2369363);
                cands.push_back({d2, oc});
              }
            }
            if (traffic && traffic->cars()) {
              for (const auto& tc : *traffic->cars()) {
                const double dx = tc.position.x - vpos.x, dy = tc.position.y - vpos.y, dz = tc.position.z - vpos.z;
                const double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > 140.0 * 140.0) continue;
                audio::OtherCarAudio oc{};
                oc.active = true;
                oc.is_traffic = true;
                oc.pos = {float(tc.position.x), float(tc.position.y), float(tc.position.z)};
                oc.vel = {float(std::sin(tc.heading) * tc.speed), 0.0f, float(std::cos(tc.heading) * tc.speed)};
                oc.rpm = float(1100.0 + std::fabs(tc.speed) * 90.0);
                oc.speed_mph = float(std::fabs(tc.speed) * 2.2369363);
                cands.push_back({d2, oc});
              }
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            const size_t n_other = std::min(cands.size(), size_t(audio::VehicleAudioState::kMaxOtherCars));
            for (size_t oi = 0; oi < n_other; ++oi) vst.other_cars[oi] = cands[oi].car;

            bool in_countdown = false;
            int countdown_digit = -1;
            if (race) {
              const auto rh = race->hud_info();
              in_countdown = rh.phase == carview::RaceHud::kCountdown;
              countdown_digit = rh.digit;
            }
            audio_host.update_vehicle(vst, race != nullptr, rewind_buf.is_rewinding(), in_countdown, countdown_digit);
          }
          int w = 0, h = 0;
          SDL_GetWindowSizeInPixels(window, &w, &h);
          // Pose car & camera BEFORE building projected world HUD markers and opponent name tags!
          const drive::Vehicle::Pose stepped = vehicle.pose();
          const bool rewind_scrub_view = rewind_buf.is_rewinding() || rewind_buf.is_paused_at_keyframe();
          if (replay_viewer.active()) {
            if (const auto* fc = replay_viewer.focused_car()) {
              drive::Vehicle::Pose rp = stepped;
              fc->apply_to_pose(rp);
              vehicle.set_pose(rp);
            }
          } else if (rewind_scrub_view) {
            vehicle.set_pose(rewind_buf.sampled_pose());
          } else {
            vehicle.set_pose(drive::Vehicle::blend(drawn_before, stepped, accumulator / drive::Vehicle::kStep));
          }
          if (race) race->set_blend(accumulator / drive::Vehicle::kStep);
          if (traffic) traffic->set_blend(accumulator / drive::Vehicle::kStep);
          wheel_poses(vehicle, poses);
          if (photo_mode.active()) {
            const float wb = photo_mode.effects().wheel_blur_omega_boost();
            for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wb;
          }
          showroom.set_wheel_poses(poses);
          showroom.set_braking(brake_glow(vehicle));
          tod_cockpit_cam = have_rig && rig.mode() == drive::CameraMode::kCockpit;
          place_world();
          // 3D Driving / Braking Line ribbon on the road (SPEC-CAR-PHYSICS).
          if (race && !grid_menu_open && !race->player_finished() &&
              driving_line_mode != drive::DrivingLineMode::kOff) {
            drive::build_driving_line(race->racing_line(), braking_table, race->player_progress(),
                                      std::max(0.0, vehicle.speed()), driving_line_mode, dl_samples);
            drive::build_driving_line_mesh(dl_samples, 0.55, 0.08, dl_verts);
            overlay_verts.clear();
            overlay_verts.reserve(dl_verts.size());
            const double ox = showroom.world_origin_x();
            const double oz = showroom.world_origin_z();
            for (const auto& dv : dl_verts) {
              carview::WorldVertex wv{};
              wv.position[0] = float(double(dv.x) - ox);
              wv.position[1] = dv.y;
              wv.position[2] = float(double(dv.z) - oz);
              wv.normal[1] = 127;
              wv.colour[0] = dv.r;
              wv.colour[1] = dv.g;
              wv.colour[2] = dv.b;
              wv.colour[3] = dv.a;
              overlay_verts.push_back(wv);
            }
            showroom.set_world_overlay(overlay_verts);
          } else {
            showroom.set_world_overlay({});
          }
          const float aspect = h > 0 ? float(w) / float(h) : 1.0f;
          drive::CameraView rw_cam{};
          drive::CameraView rv_cam{};
          if (replay_viewer.active() && !photo_mode.active()) {
            float oy = 0.0f, op = 0.0f, oz = 0.0f;
            const bool* kb = SDL_GetKeyboardState(nullptr);
            if (kb[SDL_SCANCODE_A]) oy -= 1.0f;
            if (kb[SDL_SCANCODE_D]) oy += 1.0f;
            if (kb[SDL_SCANCODE_W]) op += 1.0f;
            if (kb[SDL_SCANCODE_S]) op -= 1.0f;
            if (kb[SDL_SCANCODE_Q]) oz -= 1.0f;
            if (kb[SDL_SCANCODE_E]) oz += 1.0f;
            auto gb = [&](double x, double y, double z, double& gy) {
              drive::Hit hit;
              if (!ground || !ground->raycast({x, y, z}, {0, -1, 0}, 80.0, hit)) return false;
              gy = y - hit.distance;
              return true;
            };
            rv_cam = replay_viewer.compute_camera(frame_dt, oy, op, oz, gb);
          }
          drive::CameraView fc_cam{};
          if (trainer_freecam_active) {
            const auto& fc = slice12.trainer.free_cam;
            const double cp = std::cos(fc.pitch_rad), sp = std::sin(fc.pitch_rad);
            fc_cam.eye = fc.pos;
            fc_cam.forward = {std::sin(fc.yaw_rad) * cp, sp, std::cos(fc.yaw_rad) * cp};
            fc_cam.up = {0.0, 1.0, 0.0};
            fc_cam.fov_degrees = 60.0;
          }
          ShowroomFrame cam_frame =
              photo_mode.active()
                  ? camera_view_frame(photo_mode.camera().view(), info, vehicle, aspect, renderer.reverse_z())
                  : trainer_freecam_active
                        ? camera_view_frame(fc_cam, info, vehicle, aspect, renderer.reverse_z())
                        : replay_viewer.active()
                        ? camera_view_frame(rv_cam, info, vehicle, aspect, renderer.reverse_z())
                        : show_title
                        ? scenic_orbit_frame(info, vehicle, aspect, title_orbit_time * 0.22, 6.2f, 1.65f, renderer.reverse_z())
                        : (race && grid_menu_open)
                              ? scenic_orbit_frame(info, vehicle, aspect,
                                                   std::atan2(drive::rotate(vehicle.orientation(), {0, 0, 1}).x,
                                                              drive::rotate(vehicle.orientation(), {0, 0, 1}).z) +
                                                       0.58 + 0.12 * std::sin(title_orbit_time * 0.4),
                                                   5.6f, 1.35f, renderer.reverse_z())
                              : (rewind_scrub_view && rewind_buf.sampled_camera(rw_cam))
                                    ? camera_view_frame(rw_cam, info, vehicle, aspect, renderer.reverse_z())
                                    : drive_frame(aspect, frame_dt);
          float w_jitter_uv_x = 0.0f, w_jitter_uv_y = 0.0f;
          if (photo_mode.active() && photo_mode.accumulator().active()) {
            float jx = 0.0f, jy = 0.0f;
            photo_mode.accumulator().jitter_ndc(jx, jy);
            apply_ndc_jitter(cam_frame, jx, jy);
          } else if (renderer.active_upscaler().temporal) {
            const auto rex = renderer.render_width()
                                 ? carview::modern::Extent{renderer.render_width(), renderer.render_height()}
                                 : renderer.expected_render_extent(uint32_t(w), uint32_t(h));
            const uint32_t jphase = carview::modern::jitter_phase_count(rex, {uint32_t(w), uint32_t(h)});
            const auto j = carview::modern::halton_jitter(temporal_frame_idx++, jphase);
            apply_ndc_jitter(cam_frame, 2.0f * j[0] / float(std::max(1u, rex.w)),
                             2.0f * j[1] / float(std::max(1u, rex.h)));
            w_jitter_uv_x = j[0] / float(std::max(1u, rex.w));
            w_jitter_uv_y = j[1] / float(std::max(1u, rex.h));
          }
          if (photo_mode.active() || replay_viewer.active() || show_title || (race && grid_menu_open) ||
              ceremony.active || rewind_scrub_view)
            showroom.set_draw_car(!replay_viewer.active() || replay_viewer.camera_mode() != carview::ReplayCameraMode::kBumper);
          showroom.set_frame(cam_frame);

          // Update projected Free-Roam World Event / Festival Marker.
          active_trigger = carview::WorldTrigger{};
          world_marker = carview::WorldMarkerHud{};
          if (!race && !show_title && (have_career_catalog || have_outposts) && w > 0 && h > 0) {
            const drive::Vec3 at = vehicle.position();
            if (have_career_catalog) {
              active_trigger = carview::select_world_trigger(career_catalog, career, collected_wristband,
                                                             current_pop_rank(), at.x, at.z);
            }
            if (have_outposts) {
              for (const auto& op : outposts.outposts) {
                const double d = std::hypot(op.trigger.x - at.x, op.trigger.z - at.z);
                const double max_d = std::max(160.0, op.discovery_radius);
                if (d > max_d) continue;
                const bool in_r = d <= op.trigger_radius;
                const bool better = active_trigger.kind == carview::WorldTrigger::kNone ||
                                    (in_r && !active_trigger.in_range) ||
                                    (in_r == active_trigger.in_range && d < active_trigger.dist);
                if (better) {
                  active_trigger = carview::WorldTrigger{};
                  active_trigger.kind = carview::WorldTrigger::kOutpost;
                  active_trigger.outpost = &op;
                  active_trigger.in_range = in_r;
                  active_trigger.dist = d;
                }
              }
            }
            if (active_trigger.kind != carview::WorldTrigger::kNone) {
              std::array<double, 3> p{};
              if (active_trigger.kind == carview::WorldTrigger::kOutpost && active_trigger.outpost) {
                const auto& op = *active_trigger.outpost;
                p = {op.trigger.x, op.trigger.y, op.trigger.z};
                world_marker.is_festival = true;
                world_marker.title = op.display_name;
                int done_st = 0;
                for (const auto& mid : op.mission_ids) done_st += outpost_progress.is_stunt_completed(mid);
                world_marker.subtitle = "HORIZON OUTPOST  •  PR STUNTS " + std::to_string(done_st) + "/" +
                                        std::to_string(op.mission_ids.size());
                world_marker.prompt = "ENTER OUTPOST HUB";
                world_marker.badge = outpost_progress.is_outpost_discovered(op.id) ? "HUB" : "NEW";
              } else if (active_trigger.site) {
                p = active_trigger.site->pos;
                world_marker.is_festival = true;
                world_marker.title = active_trigger.site->name;
                const bool wb_ready = active_trigger.site->kind == race::FestivalSite::kRaceCentral &&
                                      career_catalog.pending_wristband(career.wristband_points, collected_wristband) >= 0;
                world_marker.subtitle = wb_ready ? "NEW WRISTBAND READY TO COLLECT!" : "HORIZON FESTIVAL";
                world_marker.prompt = wb_ready ? "COLLECT WRISTBAND" : "ENTER NOW";
                if (wb_ready) world_marker.badge = "NEW";
              } else if (active_trigger.hub) {
                p = active_trigger.hub->trigger_pos;
                world_marker.title = "STREET RACE HUB " + std::to_string(active_trigger.hub->id);
                world_marker.subtitle = "STREET RACING";
                world_marker.prompt = "ENTER HUB";
              } else if (active_trigger.event) {
                p = active_trigger.event->trigger_pos;
                const std::string ename = carview::game_text(active_trigger.event->name_ref);
                world_marker.title = (!ename.empty() && ename.rfind("_&", 0) != 0) ? ename : active_trigger.event->horizon_id;
                int64_t cid = 0;
                int clid = 2;
                current_car_info(cid, clid);
                const auto elig = career_catalog.check_eligibility(*active_trigger.event, cid, clid, class_name_list());
                world_marker.eligible = elig.eligible;
                world_marker.subtitle = elig.eligible
                                            ? (std::string(race::CareerCatalog::event_style_label(*active_trigger.event)) +
                                               "  •  " + carview::thousands(int64_t(active_trigger.event->cash_prize)) + " Cr")
                                            : elig.reason;
                world_marker.prompt = elig.eligible ? "ENTER NOW" : "INELIGIBLE";
                const int bp = career_catalog.best_place(*active_trigger.event, career);
                if (bp == 0) world_marker.badge = "NEW";
                else if (bp == 1) world_marker.badge = "1ST";
              }
              float sx = 0.5f * float(w), sy = 0.35f * float(h);
              const bool on_screen = project_world_point(cam_frame, info, vehicle, p[0], p[1] + 2.6, p[2], float(w),
                                                         float(h), sx, sy);
              if (on_screen || active_trigger.in_range) {
                world_marker.visible = true;
                world_marker.in_range = active_trigger.in_range;
                world_marker.screen_x = on_screen ? sx : 0.5f * float(w);
                world_marker.screen_y = on_screen ? sy : 0.32f * float(h);
                world_marker.distance_m = active_trigger.dist;
              }
            }
          }
          // Update projected Opponent Overhead Name Tags in races (race-hud.png),
          // Airborne Showcases, and free-roam Festival AI racers.
          name_tags.clear();
          if (race && !grid_menu_open && !race->player_finished() && w > 0 && h > 0) {
            std::vector<carview::RaceView::OpponentTag> otags;
            race->opponent_tags(otags);
            const drive::Vec3 at = vehicle.position();
            for (const auto& ot : otags) {
              const double d = std::hypot(ot.x - at.x, ot.z - at.z);
              if (d > 110.0) continue;
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, ot.x, ot.y + 1.85, ot.z, float(w), float(h), sx, sy))
                name_tags.push_back({sx, sy, ot.name, ot.leader});
            }
            carview::RaceView::OpponentTag stag;
            if (race->showcase_tag(stag)) {
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, stag.x, stag.y, stag.z, float(w), float(h), sx, sy))
                name_tags.push_back({sx, sy, stag.name, stag.leader});
            }
          } else if (!race && festival_mgr && w > 0 && h > 0) {
            const drive::Vec3 at = vehicle.position();
            for (const auto& fc : festival_mgr->cars()) {
              if (std::hypot(fc.position.x - at.x, fc.position.z - at.z) > 120.0) continue;
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, fc.position.x, fc.position.y + 1.85, fc.position.z,
                                      float(w), float(h), sx, sy))
                name_tags.push_back({sx, sy, "[" + fc.spec.car_class + " " + std::to_string(fc.spec.pi) + "] " + fc.spec.driver_name,
                                     fc.in_h2h});
            }
          }

          if (photo_mode.active()) {
            float cfw[16];
            compute_clip_from_world(cam_frame, info, vehicle, cfw);
            photo_mode.update_live_promo_preview(
                promo_tracker.evaluate(cfw, photo_mode.camera().eye(), gather_promo_candidates(), false));
            carview::ui::List plist;
            photo_mode.build_hud(plist, float(w), float(h));
            upload_ui_atlas(renderer, !ui_atlas_synced);
            ui_atlas_synced = true;
            renderer.set_ui(plist.items());
            for (int layer : {0, 1, 2, 3}) renderer.set_overlay({}, 0, 0, 0, 0, layer);
          } else if (replay_viewer.active() || replay_viewer.browser_open()) {
            carview::ui::List rlist;
            replay_viewer.build_hud(rlist, float(w), float(h));
            upload_ui_atlas(renderer, !ui_atlas_synced);
            ui_atlas_synced = true;
            renderer.set_ui(rlist.items());
            for (int layer : {0, 1, 2, 3}) renderer.set_overlay({}, 0, 0, 0, 0, layer);
          } else if (achievements.screen_open()) {
            carview::ui::List alist;
            achievements.build_screen_hud(alist, float(w), float(h));
            achievements.build_hud_overlays(alist, float(w), float(h), false);
            upload_ui_atlas(renderer, !ui_atlas_synced);
            ui_atlas_synced = true;
            renderer.set_ui(alist.items());
            for (int layer : {0, 1, 2, 3}) renderer.set_overlay({}, 0, 0, 0, 0, layer);
          } else if (map_open) {
            // Smooth analog-stick panning and trigger zooming on the fullscreen map.
            for (SDL_Gamepad* pad : pads) {
              const double sx = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, 0.18, 0.95);
              const double sy = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0, 0.18, 0.95);
              const double z_in = std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0);
              const double z_out = std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0);
              if (std::fabs(sx) > 0.01 || std::fabs(sy) > 0.01) {
                const double speed = (3600.0 / map_zoom) * std::clamp(frame_dt, 0.001, 0.05);
                map_cx += sx * speed;
                map_cz -= sy * speed;
                map_dirty = true;
              }
              if (z_in > 0.08 || z_out > 0.08) {
                const double zf = std::pow(2.4, (z_in - z_out) * std::clamp(frame_dt, 0.001, 0.05));
                map_zoom = std::clamp(map_zoom * zf, 1.0, 24.0);
                map_dirty = true;
              }
            }
            if (w != map_w || h != map_h) map_dirty = true;
            if (map_dirty && w > 64 && h > 64) {
              map_dirty = false;
              map_w = w, map_h = h;
              const int m = std::max(16, h / 30);
              MapScene ms = map_scene();
              ms.filter = map_filter;
              cached_map_markers = ms.markers;
              update_map_selection();
              ms.selected_marker = map_selected_marker;
              HudImage img = map_view.pause_map(ms, w - 2 * m, h - 2 * m, map_zoom, map_cx, map_cz);
              renderer.set_overlay(std::move(img.rgba), img.width, img.height, m, m, 2);
            }
            set_hud(w, h, "", nullptr);  // draws MapOverlayHud (hovered card, filters, roads counter & help bar) on UI layer!
          } else if (show_hud || show_title || ceremony.active) {
            set_minimap(h, false);
            char line[160] = "";
            const bool show_pick_banner = picking && pick < baked_cars.size();
            if (show_pick_banner)
              std::snprintf(line, sizeof line, "car %zu/%zu: %s - Enter drives it", pick + 1, baked_cars.size(),
                            carview::car_name(baked_cars[pick]).c_str());
            if (!show_title && !ceremony.active) set_radio_popup(h);
            set_hud(w, h,
                    show_pick_banner                 ? std::string(line)
                    : vehicle.time() < notice_until  ? notice
                    : race ? race_banner()
                                 : vehicle.time() < kIntroSeconds ? intro
                                                     : club_banner(),
                    &menu);
          }
          vehicle.set_pose(stepped);
          renderer.set_camera_projection(g_cam_near, g_cam_far, g_cam_proj00, g_cam_proj11);
          renderer.set_camera_vp(g_cam_vp_unjittered, w_jitter_uv_x, w_jitter_uv_y);
          const auto wf_t0 = std::chrono::steady_clock::now();
          if (!renderer.draw_frame(FrameConstants{}, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          last_frame_cpu_ms =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wf_t0).count();
          const double now_s =
              std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
          const double wait_s = frame_pacer.wait_for(now_s);
          if (wait_s > 0.0005) SDL_DelayNS(Uint64(wait_s * 1e9));
          frame_stats.frame(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
          if (renderer.active_frame_gen() != carview::modern::FrameGen::kOff) frame_stats.generated(1);
          if (window_frames_limit && frame_stats.frames() >= window_frames_limit) {
            running = false;
            const auto vram = renderer.vram_stats();
            std::printf("window: %s\nwindow: vram %.1f MiB allocated in %u allocs, %.1f MiB device-local in use\n",
                        frame_stats.summary().c_str(), double(vram.allocation_bytes) / (1024.0 * 1024.0),
                        vram.allocation_count, double(vram.device_local_usage) / (1024.0 * 1024.0));
          }
          // Process real Vulkan swapchain / color-target PNG captures (F8, F12, Photo Mode 4x SSAA).
          if (pending_shot != PendingShot::kNone) {
            std::vector<uint8_t> cap_rgba;
            uint32_t cap_w = 0, cap_h = 0;
            if (renderer.take_captured_frame(cap_rgba, cap_w, cap_h)) {
              if (pending_shot == PendingShot::kPhotoMode && photo_mode.accumulator().active()) {
                photo_mode.accumulator().add_frame(cap_rgba, cap_w, cap_h);
                if (photo_mode.accumulator().complete()) {
                  std::vector<uint8_t> final_rgba;
                  uint32_t fw = 0, fh = 0;
                  if (photo_mode.accumulator().resolve(final_rgba, fw, fh)) {
                    std::filesystem::path saved_png;
                    std::string werr;
                    if (carview::write_photo_png(photos_dir, final_rgba, fw, fh, "photo", saved_png, &werr)) {
                      float cfw[16];
                      compute_clip_from_world(cam_frame, info, vehicle, cfw);
                      const auto promo =
                          promo_tracker.evaluate(cfw, photo_mode.camera().eye(), gather_promo_candidates(), true);
                      if (promo.credits_awarded > 0) {
                        const int64_t pcr = slice12.trainer.pacing.scale_credits(promo.credits_awarded);
                        career.credits += pcr;
                        career.total_winnings += pcr;
                        save_now("horizon promo");
                      }
                      photo_mode.notify_photo_saved(saved_png, promo);
                    }
                  }
                  pending_shot = PendingShot::kNone;
                } else {
                  renderer.request_capture(false);
                }
              } else {
                const std::string pfx = pending_shot == PendingShot::kF12 ? "pose" : "shot";
                std::filesystem::path saved_png;
                std::string werr;
                if (carview::write_photo_png(photos_dir, cap_rgba, cap_w, cap_h, pfx, saved_png, &werr)) {
                  notice = "PHOTO SAVED: " + saved_png.filename().string();
                  notice_until = vehicle.time() + 3.5;
                } else {
                  notice = "PHOTO FAILED: " + werr;
                  notice_until = vehicle.time() + 3.5;
                }
                pending_shot = PendingShot::kNone;
              }
            }
          }
        }
        sync_picker = nullptr;
        slice12.shutdown(pads);
        for (SDL_Gamepad* pad : pads) {
          SDL_RumbleGamepad(pad, 0, 0, 0);
          SDL_CloseGamepad(pad);
        }
        audio_host.stop_device();
      }
      if (!map_png.empty() && status == 0) {
        if (!map_view.ready()) {
          std::fprintf(stderr, "carview: no map to write\n");
        } else {
          const std::filesystem::path prefix(map_png);
          auto name = [&](const char* suffix) {
            return (prefix.parent_path() / (prefix.stem().string() + suffix)).string();
          };
          const MapScene sc = map_scene();
          const auto c = map_view.roads_centre();
          const HudImage whole = map_view.pause_map(sc, 1600, 900, 1, c[0], c[1]);
          const HudImage close = map_view.pause_map(sc, 1600, 900, 6, sc.x, sc.z);
          const HudImage mini = map_view.minimap(sc, 300);
          bool ok = write_png(name("_map.png"), whole.width, whole.height, 4, whole.rgba.data());
          ok = ok && write_png(name("_map_near.png"), close.width, close.height, 4, close.rgba.data());
          ok = ok && write_png(name("_minimap.png"), mini.width, mini.height, 4, mini.rgba.data());
          if (!ok) status = 1;
          std::printf("maps: %s, %s, %s\n", name("_map.png").c_str(), name("_map_near.png").c_str(),
                      name("_minimap.png").c_str());
        }
      }
      if (!pause_map_png.empty() && status == 0) {
        if (!map_view.ready()) {
          std::fprintf(stderr, "carview: no map to write\n");
        } else {
          const int m = std::max(16, int(height) / 30);
          MapScene sc = map_scene();
          // Select an Outpost marker near the viewport centre if available so the card shows real Outpost metadata.
          int best_idx = -1;
          double best_d = 1e18;
          for (size_t mi = 0; mi < sc.markers.size(); ++mi) {
            if (sc.markers[mi].kind == MapMarker::kOutpost) {
              const double d = std::hypot(sc.markers[mi].x - sc.x, sc.markers[mi].z - sc.z);
              if (d < best_d) best_d = d, best_idx = int(mi);
            }
          }
          const double cx = best_idx >= 0 ? 0.5 * (sc.x + sc.markers[size_t(best_idx)].x) : sc.x;
          const double cz = best_idx >= 0 ? 0.5 * (sc.z + sc.markers[size_t(best_idx)].z) : sc.z;
          sc.selected_marker = best_idx;
          HudImage img = map_view.pause_map(sc, int(width) - 2 * m, int(height) - 2 * m, cli_map_zoom, cx, cz);
          for (int layer : {0, 1, 3}) renderer.set_overlay({}, 0, 0, 0, 0, layer);
          mm_size = 0;
          renderer.set_overlay(std::move(img.rgba), img.width, img.height, m, m, 2);
          map_overlay = carview::MapOverlayHud{};
          map_overlay.active = true;
          map_overlay.active_filter = 0;
          for (int fi = 0; fi < int(MapFilter::kCount); ++fi)
            map_overlay.filter_tabs.push_back(map_filter_name(MapFilter(fi)));
          map_overlay.roads_discovered = road_discovery.ready() ? road_discovery.discovered_count() : 0;
          map_overlay.roads_total = road_discovery.ready() ? road_discovery.total_roads() : 216;
          map_overlay.zoom = cli_map_zoom;
          map_overlay.treasure_map = reveal || treasure_map_setting;
          map_overlay.gps_active = gps_nav.active();
          map_overlay.gps_hint = gps_nav.active() ? "SET / CLEAR GPS (G)" : "SET GPS ROUTE (G)";
          if (best_idx >= 0) {
            const auto& mk = sc.markers[size_t(best_idx)];
            map_overlay.title = mk.title;
            map_overlay.subtitle = mk.subtitle;
            map_overlay.distance_m = std::hypot(mk.x - sc.x, mk.z - sc.z);
            map_overlay.action_hint = mk.discovered ? "FAST TRAVEL / HUB" : "SET GPS ROUTE";
          } else {
            map_overlay.title = "COLORADO MAP";
            map_overlay.subtitle = "PAN TO ANY ROAD, EVENT OR OUTPOST";
            map_overlay.action_hint = "FAST TRAVEL HERE";
          }
          set_hud(int(width), int(height), "");
          std::vector<uint8_t> rgba;
          renderer.set_camera_projection(g_cam_near, g_cam_far, g_cam_proj00, g_cam_proj11);
          if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
            std::fprintf(stderr, "carview: pause-map-png: %s\n", error.c_str());
            status = 1;
          } else {
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            if (!write_png(pause_map_png.string(), width, height, 4, rgba.data())) status = 1;
            else std::printf("pause map png: %s\n", pause_map_png.string().c_str());
          }
          map_overlay.active = false;
        }
      }
      if (free_roam)
        std::printf("free roam: %d of %zu signs (upgrades %d%% off), %d of %zu barn finds, %.1f km driven\n",
                    free_roam->signs_found(), collectibles.signs.size(),
                    freeroam::upgrade_discount(free_roam->signs_found()), free_roam->barns_found(),
                    collectibles.barns.size(), progress.total_distance / 1000);
      if (!skip_end_save) save_now("end");
      if (!skip_end_save && !again) save_session.on_exit(vehicle.time());
      if (traffic) std::printf("%s\n", traffic->summary().c_str());
      if (validation) std::printf("validation messages: %u\n", renderer.validation_messages());
      if (status == 0 && renderer.validation_messages()) status = 3;
    }
  }
  if (status == 0 && rally_world_switch != 0) {
    const int dir = rally_world_switch;
    rally_world_switch = 0;
    race.reset();
    race_event.clear();
    if (dir > 0 && !rally_catalog.package_dir.empty()) {
      world_dir = rally_catalog.package_dir / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "Tracks" / "ColoradoDirt";
      race_options.stage_world = world_dir;
      race_options.rally_package = rally_catalog.package_dir;
      cache_path.clear();
      have_spawn = false;
      rally_hub_open = true;
      rally_hub_selected = 0;
      rally_notice.clear();
      std::printf("rally: switched world to ColoradoDirt (%s)\n", world_dir.string().c_str());
    } else if (dir < 0 && !colorado_world_dir.empty()) {
      world_dir = colorado_world_dir;
      race_options.stage_world = world_dir;
      cache_path.clear();
      if (has_colorado_return) {
        spawn[0] = colorado_return_pos[0];
        spawn[1] = colorado_return_pos[1];
        spawn[2] = colorado_return_pos[2];
        spawn[3] = colorado_return_heading;
        have_spawn = true;
      }
      rally_session.retire();
      rally_hub_open = false;
      rally_service_open = false;
      rally_results_open = false;
      std::printf("rally: returned to Colorado (%s)\n", world_dir.string().c_str());
    }
    again = true;
  }
  if (status == 0 && leave_race) {
    leave_race = false;
    if (race) {
      race.reset();
      race_event.clear();
      std::printf("race: left for free roam\n");
    }
    if (!pre_loaner_car.empty()) {
      car = pre_loaner_car;
      if (pre_loaner_garage_id > 0) career_garage.selected = pre_loaner_garage_id;
      pre_loaner_car.clear();
      pre_loaner_garage_id = 0;
    }
    again = true;
  }
  if (status == 0 && !next_event.empty()) {
    std::string why;
    const std::string id = next_event;
    next_event.clear();
    race.reset();
    if (start_race(id, why)) {
      race_event = id;
      std::printf("race: %s from the pause menu\n", id.c_str());
    } else {
      std::fprintf(stderr, "carview: event %s: %s\n", id.c_str(), why.c_str());
      race_event.clear();
      if (!pre_loaner_car.empty()) {
        car = pre_loaner_car;
        if (pre_loaner_garage_id > 0) career_garage.selected = pre_loaner_garage_id;
        pre_loaner_car.clear();
        pre_loaner_garage_id = 0;
      }
    }
    again = true;
    SDL_SetWindowTitle(window, ("carview drive - loading " + id).c_str());
  }
  if (status == 0 && !next_car.empty()) {
    if (race) {
      std::printf("race: left for a car change\n");
      race.reset();
      race_event.clear();
      pre_loaner_car.clear();
      pre_loaner_garage_id = 0;
    }
    car = next_car;
    next_car.clear();
    again = true;
    SDL_SetWindowTitle(window, ("carview drive - loading " + car).c_str());
  }
  }
  carview::set_world_texture_session(nullptr);
  mods_session.shutdown();
  if (window) {
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
  return status;
}

}  // namespace carview
