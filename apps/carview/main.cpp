// carview: draws one FH1 car from the player's extracted disc.
//
//   carview --game-root <dir> --car <MediaName> [--lod 0-5] [--size WxH]
//           [--headless --out file.png] [--yaw deg] [--pitch deg]
//           [--distance m] [--paint r,g,b] [--msaa N] [--validation]
//           [--no-cockpit] [--all-parts] [--frames N]
//   carview --xds-png <archive.zip> <member.xds> <out.png> [level]
//   carview --world <members-dir|bin.zip> --frame <frame.txt> ...  (world_view.cpp)
//   carview --bake <carbake-output> --car <media> ...  (showroom_main.cpp: the
//           showroom over baked cars, and the reference mode)
//   carview --drive --bake <carbake-output> --car <media> ...  (showroom_main.cpp:
//           one car on flat ground with the placeholder physics of libs/drive)
//
//   carview  (no arguments: the drive in a window with its defaults: the bake
//           in out/bake, the R34, Colorado; see showroom_main.cpp)
//
// Windowed: drag with the left mouse button (or the left stick) to orbit,
// wheel (or the triggers) to zoom, Esc to quit. Headless renders one frame
// offscreen, without a window system, and writes a PNG.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "car_model.h"
#include "fh1data/archive.h"
#include "fh1data/ui_scene.h"
#include "fh1data/xds.h"
#include "gamepad_db.h"
#include "renderer.h"
#include "world_view.h"

namespace carview {
int run_showroom(int argc, char** argv);
int run_drive(int argc, char** argv);
}

namespace {

constexpr float kPi = 3.14159265358979f;

struct Vec3 {
  float x, y, z;
};
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 normalize(Vec3 a) { return a * (1.0f / std::sqrt(dot(a, a))); }

// Column-major 4x4: m[column * 4 + row].
void multiply(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
}

void look_at(Vec3 eye, Vec3 target, Vec3 up, float* m) {
  const Vec3 f = normalize(target - eye);
  const Vec3 s = normalize(cross(f, up));
  const Vec3 u = cross(s, f);
  const float v[16] = {s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0,
                       -dot(s, eye), -dot(u, eye), dot(f, eye), 1};
  std::memcpy(m, v, sizeof v);
}

// Right-handed, depth 0..1, Y flipped for Vulkan's clip space.
void perspective(float fovy, float aspect, float near_z, float far_z, float* m) {
  const float f = 1.0f / std::tan(fovy / 2);
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = f / aspect;
  m[5] = -f;
  m[10] = far_z / (near_z - far_z);
  m[11] = -1;
  m[14] = near_z * far_z / (near_z - far_z);
}

struct Camera {
  float yaw = 145.0f;   // degrees about +Y; 180 looks at the car's front (-Z)
  float pitch = 14.0f;  // degrees above the horizon
  float distance = 0;   // metres; 0 = frame the car
};

carview::FrameConstants make_constants(const carview::CarModel& car, const Camera& camera,
                                       float aspect, const float paint[3]) {
  const Vec3 low{car.bounds_min[0], car.bounds_min[1], car.bounds_min[2]};
  const Vec3 high{car.bounds_max[0], car.bounds_max[1], car.bounds_max[2]};
  const Vec3 center = (low + high) * 0.5f;
  const float radius = std::sqrt(dot(high - low, high - low)) * 0.5f;
  const float fovy = 35.0f * kPi / 180.0f;
  const float distance = camera.distance > 0 ? camera.distance : radius / std::tan(fovy / 2) * 0.8f;
  const float yaw = camera.yaw * kPi / 180.0f, pitch = camera.pitch * kPi / 180.0f;
  const Vec3 eye = center + Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                 std::cos(yaw) * std::cos(pitch)} * distance;
  float view[16], projection[16];
  look_at(eye, center, {0, 1, 0}, view);
  perspective(fovy, aspect, std::max(0.05f, distance - radius * 2), distance + radius * 2,
              projection);
  carview::FrameConstants c{};
  multiply(projection, view, c.view_projection);
  const Vec3 light = normalize({0.4f, 0.9f, -0.5f});
  c.light_direction[0] = light.x;
  c.light_direction[1] = light.y;
  c.light_direction[2] = light.z;
  std::memcpy(c.paint, paint, 3 * sizeof(float));
  c.eye[0] = eye.x;
  c.eye[1] = eye.y;
  c.eye[2] = eye.z;
  return c;
}

void usage() {
  std::fprintf(stderr,
               "usage: carview --game-root <dir> --car <MediaName> [--lod 0-5] [--size WxH]\n"
               "               [--headless --out file.png] [--yaw deg] [--pitch deg]\n"
               "               [--distance m] [--paint r,g,b] [--msaa N] [--validation]\n"
               "               [--no-cockpit] [--all-parts] [--frames N]\n");
}

}  // namespace

// carview --xds-png <archive.zip> <member.xds> <out.png> [level]: writes one
// decoded texture level, for checking the decoder by eye.
int xds_png(int argc, char** argv) {
  std::string error;
  auto archive = fh1::Archive::open(argv[2], &error);
  if (!archive) {
    std::fprintf(stderr, "carview: %s\n", error.c_str());
    return 1;
  }
  auto bytes = archive->read(argv[3]);
  if (!bytes.ok()) {
    std::fprintf(stderr, "carview: %s\n", bytes.error.c_str());
    return 1;
  }
  const fh1::XdsResult texture = fh1::decode_xds(bytes.data, true);
  const size_t level = argc > 5 ? size_t(std::atoi(argv[5])) : 0;
  if (level >= texture.levels.size()) {
    std::fprintf(stderr, "carview: %s (%zu levels)\n", texture.error.c_str(), texture.levels.size());
    return 1;
  }
  const fh1::RgbaImage& image = texture.levels[level];
  std::printf("%s: format %u (%s), %ux%u, %zu levels, swizzle %03o, endian %u\n", argv[3],
              texture.header.format, fh1::xenos_format_name(texture.header.format),
              texture.header.width, texture.header.height, texture.levels.size(),
              texture.header.swizzle, texture.header.endian);
  return stbi_write_png(argv[4], int(image.width), int(image.height), 4, image.rgba.data(),
                        int(image.width * 4))
             ? 0
             : 1;
}

// carview --ui-scene <UI.zip> <scene>: the scene's objects and nodes
// (FORMAT-UISCENES), one line each, for checking the reader.
int ui_scene_dump(int argc, char** argv) {
  std::string error;
  auto zip = fh1::Archive::open(argv[2], &error);
  if (!zip) return std::fprintf(stderr, "carview: %s\n", error.c_str()), 1;
  const std::string base = std::string("Scenes/ui4/") + argv[3];
  auto fbf = zip->read(base + ".fbf");
  auto bsg = zip->read(base + ".bsg");
  if (!fbf.ok() || !bsg.ok()) return std::fprintf(stderr, "carview: %s%s\n", fbf.error.c_str(), bsg.error.c_str()), 1;
  std::printf("bsg header:");
  for (size_t i = 0; i < 48 && i < bsg.data.size(); ++i) std::printf("%s%02x", i % 4 ? "" : " ", bsg.data[i]);
  std::printf("\n");
  fh1::UiScene sc;
  if (!fh1::parse_ui_fbf(fbf.data, sc, &error) || !fh1::parse_ui_bsg(bsg.data, sc, &error))
    return std::fprintf(stderr, "carview: %s\n", error.c_str()), 1;
  std::printf("version %u, %zu objects, %zu nodes, %zu pool\n", sc.version, sc.objects.size(), sc.nodes.size(), sc.pool.size());
  const bool all = argc > 4;
  for (size_t i = 0; i < sc.objects.size() && (all || i < 400); ++i) {
    const auto& o = sc.objects[i];
    std::printf("obj %zu kind %u id %u %s", i, uint32_t(o.kind), o.id, o.name.c_str());
    if (o.kind == fh1::UiObjectKind::kText) std::printf(" font %s size %u align %u width %.0f", o.font.c_str(), o.text_size, o.text_align, o.text_width);
    if (o.kind == fh1::UiObjectKind::kMaterial) std::printf(" rgba %.2f %.2f %.2f %.2f", o.colour[0], o.colour[1], o.colour[2], o.colour[3]);
    if (o.kind == fh1::UiObjectKind::kTexture) std::printf(" owner %u %s m %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g", o.owner, o.path.c_str(),
        o.matrix[0], o.matrix[1], o.matrix[2], o.matrix[3], o.matrix[4], o.matrix[5], o.matrix[6], o.matrix[7], o.matrix[8], o.matrix[9], o.matrix[10], o.matrix[11], o.matrix[12], o.matrix[13], o.matrix[14], o.matrix[15]);
    std::printf("\n");
  }
  for (size_t i = 0; i < sc.nodes.size() && (all || i < 400); ++i) {
    const auto& n = sc.nodes[i];
    const auto* o = n.object_index >= 0 ? &sc.objects[size_t(n.object_index)] : nullptr;
    const float hw = 0.5f * (n.bounds_max[0] - n.bounds_min[0]) * n.sx, hh = 0.5f * (n.bounds_max[1] - n.bounds_min[1]) * n.sy;
    std::printf("node %zu parent %d obj %u kind %d %s at %.1f,%.1f rot %.3f scale %.3f,%.3f alpha %.2f%s rect %.0f,%.0f-%.0f,%.0f\n", i, n.parent, n.object,
                o ? int(o->kind) : -1, o ? o->name.c_str() : "?", 640 + n.x, 360 - n.y, n.angle, n.sx, n.sy, n.alpha,
                n.drawable ? " draw" : "", 640 + n.x - hw, 360 - n.y - hh, 640 + n.x + hw, 360 - n.y + hh);
  }
  return 0;
}

// carview --ui-scene-png <UI.zip> <scene> <out.png>: the scene's static
// layout drawn on the CPU (FORMAT-UISCENES interims: orthographic, the
// BSG's opacity as the state, a model's material and textures multiplied,
// text boxes outlined), for checking the reader against frames.
int ui_scene_png(int argc, char** argv) {
  (void)argc;
  std::string error;
  const std::filesystem::path ui_zip = argv[2];
  auto zip = fh1::Archive::open(ui_zip, &error);
  if (!zip) return std::fprintf(stderr, "carview: %s\n", error.c_str()), 1;
  const std::string base = std::string("Scenes/ui4/") + argv[3];
  auto fbf = zip->read(base + ".fbf");
  auto bsg = zip->read(base + ".bsg");
  fh1::UiScene sc;
  if (!fbf.ok() || !bsg.ok() || !fh1::parse_ui_fbf(fbf.data, sc, &error) || !fh1::parse_ui_bsg(bsg.data, sc, &error))
    return std::fprintf(stderr, "carview: %s%s%s\n", fbf.error.c_str(), bsg.error.c_str(), error.c_str()), 1;
  const auto media = ui_zip.parent_path();
  auto horizon = fh1::Archive::open(media / "ui" / "textures" / "Horizon.zip", &error);
  auto textures = fh1::Archive::open(media / "ui" / "Textures.zip", &error);
  std::map<std::string, fh1::RgbaImage> cache;
  auto texture = [&](const std::string& path) -> const fh1::RgbaImage* {
    if (auto it = cache.find(path); it != cache.end()) return it->second.width ? &it->second : nullptr;
    bool hz = false;
    const std::string member = fh1::ui_texture_member(path, hz);
    fh1::RgbaImage& img = cache[path];
    auto* a = hz ? horizon.get() : textures.get();
    if (!a) return nullptr;
    auto r = a->read(member);
    if (!r.ok()) return nullptr;
    auto x = fh1::decode_xds(r.data, false);
    if (x.levels.empty()) return nullptr;
    img = std::move(x.levels[0]);
    return &img;
  };
  const int W = 1280, H = 720;
  std::vector<float> px(size_t(W) * H * 3, 0.35f);
  auto blend = [&](int x, int y, const float c[4]) {
    if (x < 0 || y < 0 || x >= W || y >= H || c[3] <= 0) return;
    float* p = &px[(size_t(y) * W + size_t(x)) * 3];
    for (int k = 0; k < 3; ++k) p[k] = c[k] * c[3] + p[k] * (1 - c[3]);
  };
  size_t drawn = 0, missing = 0;
  for (size_t i = 0; i < sc.nodes.size(); ++i) {
    const fh1::UiNode& n = sc.nodes[i];
    if (n.object_index < 0 || n.alpha <= 0.001f) continue;
    const fh1::UiObject& o = sc.objects[size_t(n.object_index)];
    const float c = std::cos(n.angle), s = std::sin(n.angle);
    const float cx = 640 + n.x, cy = 360 - n.y;
    if (o.kind == fh1::UiObjectKind::kText) {
      const float w = o.text_width > 0 ? o.text_width * n.sx : float(o.text_size) * 4, h = float(o.text_size) * n.sy;
      const float x0 = o.text_align == 1 ? cx - w / 2 : o.text_align == 2 ? cx - w : cx;
      const float col[4] = {1, 0.9f, 0.1f, 0.8f * n.alpha};
      for (int x = int(x0); x < int(x0 + w); ++x) blend(x, int(cy), col), blend(x, int(cy + h), col);
      for (int y = int(cy); y < int(cy + h); ++y) blend(int(x0), y, col), blend(int(x0 + w), y, col);
      continue;
    }
    if (o.kind != fh1::UiObjectKind::kModel) continue;
    // The material and its textures follow the model in the FBF.
    const fh1::UiObject* mat = nullptr;
    std::vector<const fh1::UiObject*> tex;
    for (size_t k = size_t(n.object_index) + 1; k < sc.objects.size(); ++k) {
      const auto& q = sc.objects[k];
      if (q.kind == fh1::UiObjectKind::kMaterial && !mat) mat = &q;
      else if (q.kind == fh1::UiObjectKind::kTexture && mat && q.owner == mat->id) tex.push_back(&q);
      else break;
    }
    std::vector<const fh1::RgbaImage*> img;
    for (auto* t : tex) {
      img.push_back(texture(t->path));
      if (!img.back()) ++missing;
    }
    const float bx0 = n.bounds_min[0], bx1 = n.bounds_max[0], by0 = n.bounds_min[1], by1 = n.bounds_max[1];
    if (bx1 <= bx0 || by1 <= by0) continue;
    const float hw = 0.5f * (bx1 - bx0) * std::fabs(n.sx), hh = 0.5f * (by1 - by0) * std::fabs(n.sy);
    const float r = std::sqrt(hw * hw + hh * hh);
    ++drawn;
    for (int y = int(cy - r); y <= int(cy + r); ++y)
      for (int x = int(cx - r); x <= int(cx + r); ++x) {
        // Into the node's frame (y up), then to (u, v) over the bounds.
        const float dx = float(x) + 0.5f - cx, dy = cy - (float(y) + 0.5f);
        const float lx = (c * dx + s * dy) / n.sx, ly = (-s * dx + c * dy) / n.sy;
        const float u = (lx - bx0) / (bx1 - bx0), v = (ly - by0) / (by1 - by0);
        if (u < 0 || u > 1 || v < 0 || v > 1) continue;
        float col[4] = {1, 1, 1, n.alpha};
        if (mat)
          for (int k = 0; k < 4; ++k) col[k] *= mat->colour[size_t(k)];
        for (size_t t = 0; t < tex.size(); ++t) {
          if (!img[t]) continue;
          const auto& m = tex[t]->matrix;
          float tu = u * m[0] + v * m[4] + m[12], tv = u * m[1] + v * m[5] + m[13];
          tu -= std::floor(tu), tv -= std::floor(tv);
          const auto& im = *img[t];
          const uint32_t ix = std::min(im.width - 1, uint32_t(tu * float(im.width)));
          const uint32_t iy = std::min(im.height - 1, uint32_t(tv * float(im.height)));
          const uint8_t* tp = &im.rgba[(size_t(iy) * im.width + ix) * 4];
          for (int k = 0; k < 4; ++k) col[k] *= float(tp[k]) / 255.0f;
        }
        blend(x, y, col);
      }
  }
  std::vector<uint8_t> out(size_t(W) * H * 3);
  for (size_t k = 0; k < out.size(); ++k) out[k] = uint8_t(std::lround(std::clamp(px[k], 0.0f, 1.0f) * 255));
  std::printf("%s: %zu models drawn, %zu textures missing\n", argv[3], drawn, missing);
  return stbi_write_png(argv[4], W, H, 3, out.data(), W * 3) ? 0 : 1;
}

static int carview_main(int argc, char** argv) {
  // No arguments: the drive with its play-test defaults (run_drive).
  if (argc == 1) {
    SDL_SetMainReady();
    return carview::run_drive(argc, argv);
  }
  if (argc >= 5 && std::strcmp(argv[1], "--xds-png") == 0) return xds_png(argc, argv);
  if (argc >= 4 && std::strcmp(argv[1], "--ui-scene") == 0) return ui_scene_dump(argc, argv);
  if (argc >= 5 && std::strcmp(argv[1], "--ui-scene-png") == 0) return ui_scene_png(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "--world") == 0) return carview::run_world(argc, argv);
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--drive") == 0) {
      SDL_SetMainReady();
      return carview::run_drive(argc, argv);
    }
  // The drive is the default mode. The showroom: --bake with neither a world
  // nor a play folder (which only the drive takes); the disc viewer below:
  // --game-root.
  bool bake = false, drive_world = false, disc_viewer = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bake") == 0) bake = true;
    if (std::strcmp(argv[i], "--world") == 0 || std::strcmp(argv[i], "--play") == 0) drive_world = true;
    if (std::strcmp(argv[i], "--game-root") == 0) disc_viewer = true;
  }
  if (bake && !drive_world) {
    SDL_SetMainReady();
    return carview::run_showroom(argc, argv);
  }
  if (!disc_viewer) {
    SDL_SetMainReady();
    return carview::run_drive(argc, argv);
  }
  std::filesystem::path game_root;
  std::string car, out = "carview.png";
  int lod = 0;
  uint32_t width = 1280, height = 720, msaa = 4, max_frames = 0;
  bool headless = false, validation = std::getenv("SHIFT_VALIDATION") != nullptr;
  Camera camera;
  carview::LoadOptions load_options;
  float paint[3] = {0.62f, 0.05f, 0.04f};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--headless") headless = true;
    else if (a == "--validation") validation = true;
    else if (a == "--no-cockpit") load_options.cockpit = false;
    else if (a == "--all-parts") load_options.all_parts = true;
    else if (a == "--frames" && (v = next())) max_frames = uint32_t(std::atoi(v));
    else if (a == "--game-root" && (v = next())) game_root = v;
    else if (a == "--car" && (v = next())) car = v;
    else if (a == "--out" && (v = next())) out = v;
    else if (a == "--lod" && (v = next())) lod = std::atoi(v);
    else if (a == "--msaa" && (v = next())) msaa = uint32_t(std::atoi(v));
    else if (a == "--yaw" && (v = next())) camera.yaw = float(std::atof(v));
    else if (a == "--pitch" && (v = next())) camera.pitch = float(std::atof(v));
    else if (a == "--distance" && (v = next())) camera.distance = float(std::atof(v));
    else if (a == "--size" && (v = next())) {
      if (std::sscanf(v, "%ux%u", &width, &height) != 2 || !width || !height) {
        usage();
        return 2;
      }
    } else if (a == "--paint" && (v = next())) {
      std::sscanf(v, "%f,%f,%f", &paint[0], &paint[1], &paint[2]);
    } else {
      usage();
      return 2;
    }
  }
  if (game_root.empty() || car.empty()) {
    usage();
    return 2;
  }

  carview::CarModel model;
  std::string error;
  if (!carview::load_car(game_root, car, lod, load_options, model, error)) {
    std::fprintf(stderr, "carview: %s\n", error.c_str());
    return 1;
  }
  std::printf("%s: %zu parts drawn (%zu hidden), %zu sub-sections (%zu without vertices), "
              "%zu vertices, %zu triangles; %s %ux%u, %zu levels\n",
              model.model_member.c_str(), model.parts, model.hidden_parts, model.subsections,
              model.skipped_subsections, model.vertices.size(), model.indices.size() / 3,
              model.texture_member.c_str(), model.texture_levels[0].width,
              model.texture_levels[0].height, model.texture_levels.size());
  std::printf("bounds: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f m\n", model.bounds_min[0],
              model.bounds_max[0], model.bounds_min[1], model.bounds_max[1], model.bounds_min[2],
              model.bounds_max[2]);

  if (headless) {
    carview::Renderer renderer;
    if (!renderer.init({nullptr, validation, msaa}, error) || !renderer.upload(model, error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      return 1;
    }
    std::vector<uint8_t> rgba;
    const auto constants = make_constants(model, camera, float(width) / float(height), paint);
    if (!renderer.render_offscreen(constants, width, height, rgba, error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      return 1;
    }
    if (!stbi_write_png(out.c_str(), int(width), int(height), 4, rgba.data(), int(width * 4))) {
      std::fprintf(stderr, "carview: cannot write %s\n", out.c_str());
      return 1;
    }
    std::printf("rendered %ux%u on %s (%ux MSAA), wrote %s%s\n", width, height,
                renderer.device_name().c_str(), renderer.samples(), out.c_str(),
                validation ? (" — validation messages: " +
                              std::to_string(renderer.validation_messages())).c_str()
                           : "");
    return renderer.validation_messages() ? 3 : 0;
  }

  SDL_SetMainReady();
  carview::apply_sdl_gamepad_hints();
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
    std::fprintf(stderr, "carview: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  carview::load_gamepad_mappings();
  SDL_Window* window = SDL_CreateWindow(("carview - " + car).c_str(), int(width), int(height),
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                            SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    std::fprintf(stderr, "carview: SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  int status = 0;
  {
    carview::Renderer renderer;
    if (!renderer.init({window, validation, msaa}, error) || !renderer.upload(model, error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      status = 1;
    } else {
      std::printf("device: %s (%ux MSAA)\n", renderer.device_name().c_str(), renderer.samples());
      std::vector<SDL_Gamepad*> pads;
      bool dragging = false, running = true;
      float zoom = 1.0f;
      uint64_t last = SDL_GetTicksNS();
      for (uint32_t frame = 0; running && (!max_frames || frame < max_frames); ++frame) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
          switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
              if (e.key.key == SDLK_ESCAPE) running = false;
              break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
              if (e.button.button == SDL_BUTTON_LEFT) dragging = true;
              break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
              if (e.button.button == SDL_BUTTON_LEFT) dragging = false;
              break;
            case SDL_EVENT_MOUSE_MOTION:
              if (dragging) {
                camera.yaw -= e.motion.xrel * 0.3f;
                camera.pitch = std::clamp(camera.pitch + e.motion.yrel * 0.3f, -85.0f, 85.0f);
              }
              break;
            case SDL_EVENT_MOUSE_WHEEL:
              zoom = std::clamp(zoom * std::pow(0.9f, e.wheel.y), 0.2f, 5.0f);
              break;
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
          camera.yaw -= (axis(SDL_GAMEPAD_AXIS_LEFTX) + axis(SDL_GAMEPAD_AXIS_RIGHTX)) * 120 * dt;
          camera.pitch = std::clamp(
              camera.pitch + (axis(SDL_GAMEPAD_AXIS_LEFTY) + axis(SDL_GAMEPAD_AXIS_RIGHTY)) * 90 * dt,
              -85.0f, 85.0f);
          const float z = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) - axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
          zoom = std::clamp(zoom * std::pow(2.0f, z * dt), 0.2f, 5.0f);
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        Camera view = camera;
        const float base = [&] {
          const float dx = model.bounds_max[0] - model.bounds_min[0];
          const float dy = model.bounds_max[1] - model.bounds_min[1];
          const float dz = model.bounds_max[2] - model.bounds_min[2];
          const float radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
          return radius / std::tan(35.0f * kPi / 360.0f) * 0.8f;
        }();
        view.distance = (camera.distance > 0 ? camera.distance : base) * zoom;
        const auto constants =
            make_constants(model, view, h > 0 ? float(w) / float(h) : 1.0f, paint);
        if (!renderer.draw_frame(constants, error)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
          break;
        }
      }
      for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
      if (validation) std::printf("validation messages: %u\n", renderer.validation_messages());
      if (status == 0 && renderer.validation_messages()) status = 3;
    }
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return status;
}

int main(int argc, char** argv) { return carview_main(argc, argv); }

#ifdef SDL_PLATFORM_ANDROID
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

static void* android_log_bridge(void* arg) {
  int fd = *static_cast<int*>(arg);
  delete static_cast<int*>(arg);
  char buf[1024];
  std::string line;
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
    line.append(buf, size_t(n));
    size_t pos;
    while ((pos = line.find('\n')) != std::string::npos) {
      std::string msg = line.substr(0, pos);
      if (!msg.empty() && msg.back() == '\r') msg.pop_back();
      __android_log_print(ANDROID_LOG_INFO, "ShiftEngine", "%s", msg.c_str());
      line.erase(0, pos + 1);
    }
  }
  if (!line.empty()) __android_log_print(ANDROID_LOG_INFO, "ShiftEngine", "%s", line.c_str());
  ::close(fd);
  return nullptr;
}

static void start_android_log_bridge() {
  static bool started = false;
  if (started) return;
  started = true;
  int pfd[2];
  if (::pipe(pfd) == 0) {
    ::dup2(pfd[1], STDOUT_FILENO);
    ::dup2(pfd[1], STDERR_FILENO);
    ::close(pfd[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    pthread_t th;
    if (::pthread_create(&th, nullptr, android_log_bridge, new int(pfd[0])) == 0) ::pthread_detach(th);
  }
}

// SDL's activity looks SDL_main up in libmain.so (SDL_MAIN_HANDLED above
// keeps main's own name on the desktops).
extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char** argv) {
  start_android_log_bridge();
  const int rc = carview_main(argc, argv);
  __android_log_print(ANDROID_LOG_INFO, "ShiftEngine", "carview_main exited with code %d", rc);
  std::fflush(stdout);
  std::fflush(stderr);
  return rc;
}
#endif
