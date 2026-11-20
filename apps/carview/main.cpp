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
#include "fh1data/xds.h"
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

int main(int argc, char** argv) {
  // No arguments: the drive with its play-test defaults (run_drive).
  if (argc == 1) {
    SDL_SetMainReady();
    return carview::run_drive(argc, argv);
  }
  if (argc >= 5 && std::strcmp(argv[1], "--xds-png") == 0) return xds_png(argc, argv);
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
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "carview: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
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
