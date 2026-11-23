#include "drive_helpers_runtime.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <stb_image.h>
#include <stb_image_write.h>

#include "audio/xml.h"
#include "cars/mods.h"
#include "fh1data/xds.h"
#include "hud_scene.h"
#include "mods_page.h"
#include "render_modern.h"
#include "ui.h"

namespace carview {

bool parse_light(const std::string& text, LightSettings& out) {
  std::stringstream list(text);
  for (std::string item; std::getline(list, item, ',');) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) return false;
    const std::string name = item.substr(0, eq);
    const float value = std::strtof(item.c_str() + eq + 1, nullptr);
    if (name == "exposure") out.exposure = value;
    else if (name == "env") out.environment = value;
    else if (name == "key") out.key = value;
    else if (name == "fill") out.fill = value;
    else if (name == "r") out.tint[0] = value;
    else if (name == "g") out.tint[1] = value;
    else if (name == "b") out.tint[2] = value;
    else if (name == "rr") out.reflect[0] = value;
    else if (name == "rg") out.reflect[1] = value;
    else if (name == "rb") out.reflect[2] = value;
    else if (name == "metal") out.metallic = value;
    else return false;
  }
  return true;
}

void lighting(ShowroomFrame& f, bool livery, const LightSettings& light) {
  const V3 key = norm({0.35f, 1.0f, -0.45f}), fill = norm({-0.7f, 0.35f, 0.6f});
  f.key_light[0] = key.x;
  f.key_light[1] = key.y;
  f.key_light[2] = key.z;
  f.key_light[3] = light.key;
  f.fill_light[0] = fill.x;
  f.fill_light[1] = fill.y;
  f.fill_light[2] = fill.z;
  f.fill_light[3] = light.fill;
  f.options[0] = light.exposure;
  f.options[1] = light.environment;
  f.options[2] = livery ? 1.0f : 0.0f;
  f.options[3] = light.metallic;
  for (int i = 0; i < 3; ++i) {
    f.tint[i] = light.tint[i];
    f.reflect_tint[i] = light.reflect[i];
  }
  f.tint[3] = f.reflect_tint[3] = 1.0f;
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
bool load_ui_fonts(const std::filesystem::path& game_root, UiFontCache& cache) {
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
  auto st = cache.state;
  // The game's UI textures by path, on first use (FORMAT-UISCENES).
  carview::set_ui_textures([root = game_root, st](const std::string& path, std::array<float, 4>& uv) {
    if (carview::ui::atlas_find(path, uv)) return true;
    if (st->failed_textures.count(path)) return false;
    if (!st->textures_opened) {
      st->textures_opened = true;
      std::string e;
      st->horizon_zip = fh1::Archive::open(root / "media" / "ui" / "textures" / "Horizon.zip", &e);
      st->textures_zip = fh1::Archive::open(root / "media" / "ui" / "Textures.zip", &e);
    }
    const bool t = path.rfind("T:", 0) == 0;
    const std::string rel = (t ? path.substr(2) : path) + ".xds";
    fh1::Archive* a = t ? st->textures_zip.get() : st->horizon_zip.get();
    fh1::ReadResult r = a ? a->read(rel) : fh1::ReadResult{};
    if (!r.ok() && !t) {
      std::error_code ec;
      const std::filesystem::path loose = root / "media" / "ui" / "textures" / "Horizon" / rel;
      if (std::filesystem::is_regular_file(loose, ec)) {
        std::ifstream f(loose, std::ios::binary);
        r.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      } else {
        if (!st->rally_probed) {
          st->rally_probed = true;
          const std::filesystem::path bases[] = {
              root / ".." / ".." / "dlc",
              root / ".." / "dlc",
              root / "dlc",
          };
          for (const auto& base : bases) {
            if (!std::filesystem::exists(base, ec)) continue;
            for (const auto& ent : std::filesystem::recursive_directory_iterator(base, ec)) {
              if (ent.is_regular_file(ec) && ent.path().filename() == "CoDriverIconSet.xds") {
                st->rally_tex_dir = ent.path().parent_path().parent_path().parent_path();
                break;
              }
            }
            if (!st->rally_tex_dir.empty()) break;
          }
        }
        if (!st->rally_tex_dir.empty()) {
          const std::filesystem::path p = st->rally_tex_dir / rel;
          if (std::filesystem::is_regular_file(p, ec)) {
            std::ifstream f(p, std::ios::binary);
            r.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
          }
        }
      }
    }
    const fh1::XdsResult x = !r.data.empty() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
    if (x.levels.empty() || !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
      st->failed_textures.insert(path);
      return false;
    }
    return true;
  });
  // The UI scenes by name, on first use (FORMAT-UISCENES: media/UI.zip Scenes/ui4/<name>.fbf and .bsg).
  carview::set_ui_scenes([root = game_root, st](const std::string& name) -> const fh1::UiScene* {
    if (auto it = st->scene_cache.find(name); it != st->scene_cache.end()) return it->second.get();
    if (!st->ui_opened) {
      st->ui_opened = true;
      std::string e;
      st->ui_zip = fh1::Archive::open(root / "media" / "UI.zip", &e);
    }
    auto& slot = st->scene_cache[name];
    if (!st->ui_zip) return nullptr;
    auto fbf = st->ui_zip->read("Scenes/ui4/" + name + ".fbf");
    auto bsg = st->ui_zip->read("Scenes/ui4/" + name + ".bsg");
    auto bgf = st->ui_zip->read("Scenes/ui4/" + name + ".bgf");
    auto sc = std::make_unique<fh1::UiScene>();
    std::string e;
    if (fbf.ok() && bsg.ok() && fh1::parse_ui_fbf(fbf.data, *sc, &e) && fh1::parse_ui_bsg(bsg.data, *sc, &e)) {
      if (bgf.ok()) sc->has_bgf = fh1::parse_ui_bgf(bgf.data, sc->bgf, nullptr);
      slot = std::move(sc);
    } else
      std::fprintf(stderr, "carview: note: UI scene %s not loaded%s%s\n", name.c_str(), e.empty() ? "" : ": ", e.c_str());
    return slot.get();
  });
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
void upload_ui_atlas(Renderer& renderer, bool full) {
  if (carview::ui::gpu_atlas_mode()) {
    for (auto& patch : carview::ui::atlas_take_dirty_patches()) {
      renderer.ui_atlas_put(patch.x, patch.y, patch.w, patch.h, patch.rgba);
    }
    return;
  }
  const auto& a = carview::ui::atlas_pixels();
  const uint32_t side = carview::ui::atlas_size();
  if (full) {
    carview::ui::atlas_take_dirty();
    renderer.ui_atlas_put(0, 0, side, side, a);
    return;
  }
  std::vector<uint8_t> rows;
  for (const auto& [x, y, w, h] : carview::ui::atlas_take_dirty()) {
    rows.resize(size_t(w) * h * 4);
    for (uint32_t r = 0; r < h; ++r)
      std::memcpy(&rows[size_t(r) * w * 4], &a[(size_t(y + r) * side + x) * 4], size_t(w) * 4);
    renderer.ui_atlas_put(x, y, w, h, rows);
  }
}

// A menu page (HudState's menu fields) on the UI layer, 1280 x 720, to a PNG.
bool write_menu_png(const HudState& st, const std::string& path, const carview::CareerBoard* board) {
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

void multiply4(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
}

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
V3 transform_v3(const float* m, V3 p, float w) {
  return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12] * w, m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13] * w,
          m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] * w};
}

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

void WorldTextureLoader::load(world::World& colorado, WorldTriangles& w, Showroom& showroom,
                              std::vector<uint32_t>& pin_records) {
    std::unordered_set<uint32_t> batch_seen;
    std::vector<uint32_t> to_decode;
    for (auto& m : w.materials) {
      for (uint32_t* slot :
           {&m.texture, &m.layer_b, &m.splat, &m.light_map, &m.normal_map, &m.ao_map, &m.layer_c, &m.spec_map}) {
        uint32_t& record = *slot;
        if (record == kNoWorldTexture) continue;
        if (failed.count(record)) {
          record = kNoWorldTexture;
          continue;
        }
        if (!batch_seen.insert(record).second) continue;
        if (showroom.reserve_world_texture(record)) {
          pin_records.push_back(record);
          continue;
        }
        to_decode.push_back(record);
      }
    }
    if (to_decode.empty()) return;

    struct DecodedTex {
      bool ok = false;
      bool is_cutout = false;
      std::string err;
      std::shared_ptr<WorldTextureData> data;
    };
    std::vector<DecodedTex> results(to_decode.size());
    const size_t threads =
        std::min<size_t>(to_decode.size(), std::max<size_t>(1, std::thread::hardware_concurrency()));
    auto decode_one = [&](size_t i) {
      const uint32_t record = to_decode[i];
      fh1::WorldTexture t;
      std::string e;
      if (!colorado.read_texture(record, t, &e)) {
        results[i].err = std::move(e);
        return;
      }
      results[i].ok = true;
      results[i].is_cutout = fh1::world_texture_has_cutout(t);
      auto d = std::make_shared<WorldTextureData>();
      d->record = record;
      d->srgb = t.format.gamma;
      char name[40];
      std::snprintf(name, sizeof name, "colorado:_0x%08x", colorado.textures()[record].id);
      d->texture.name = name;
      d->texture.format = cars::bake::TextureFormat(uint32_t(t.blocks.format));
      d->texture.swizzle = t.blocks.swizzle;
      for (auto& l : t.blocks.levels) d->texture.levels.push_back({l.width, l.height, std::move(l.blocks)});
      results[i].data = std::move(d);
    };
    if (threads > 1 && to_decode.size() >= 4) {
      std::atomic<size_t> next{0};
      std::vector<std::thread> workers;
      workers.reserve(threads);
      for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
          for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= to_decode.size()) break;
            decode_one(i);
          }
        });
      }
      for (auto& wk : workers) wk.join();
    } else {
      for (size_t i = 0; i < to_decode.size(); ++i) decode_one(i);
    }

    bool any_failed = false;
    for (size_t i = 0; i < to_decode.size(); ++i) {
      const uint32_t record = to_decode[i];
      auto& r = results[i];
      if (!r.ok || !r.data) {
        std::fprintf(stderr, "carview: texture record %u: %s\n", record, r.err.c_str());
        failed.insert(record);
        any_failed = true;
        continue;
      }
      if (r.is_cutout) cutout.insert(record);
      carview::world_texture_hook(record, r.data->srgb, r.data->texture);
      w.textures.push_back(std::move(r.data));
      ++decoded;
    }
    if (any_failed) {
      for (auto& m : w.materials) {
        for (uint32_t* slot :
             {&m.texture, &m.layer_b, &m.splat, &m.light_map, &m.normal_map, &m.ao_map, &m.layer_c, &m.spec_map}) {
          if (*slot != kNoWorldTexture && failed.count(*slot)) *slot = kNoWorldTexture;
        }
      }
    }
  }

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
// How far out the tree sprites / scenery are gathered and post-chain defaults
// live as inline constexpr constants in drive_helpers_runtime.h.

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

}  // namespace carview
