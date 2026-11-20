// carview --bake: the showroom over carbake's output, windowed or headless,
// the reference mode, and the drive mode (carview --drive, below).
//
//   carview --bake <dir> --car <media> [--colour N] [--livery] [--all-variants]
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
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include "audio/bake.h"
#include "hud.h"
#include "reference.h"
#include "renderer.h"
#include "showroom.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "drive/vehicle.h"
#include "world/road_follow.h"
#include "world/stream.h"
#include "world/world.h"

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
               "usage: carview --bake <dir> --car <media> [--colour N] [--livery] [--all-variants]\n"
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
        for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
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

// A camera at eye looking at target, in the car's model frame.
ShowroomFrame look_frame(V3 eye, V3 target, V3 up, float aspect, bool livery) {
  const float fovy = 55.0f * kPi / 180.0f;
  const V3 f = norm(sub(target, eye));
  const V3 side = norm(cross3(f, up));
  const V3 u = cross3(side, f);
  const float view[16] = {side.x, u.x, -f.x, 0, side.y, u.y, -f.y, 0, side.z, u.z, -f.z, 0,
                          -dot3(side, eye), -dot3(u, eye), dot3(f, eye), 1};
  const float near_z = 0.25f, far_z = 4000.0f;
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
struct CameraPull {
  double fraction = 1;
};

ShowroomFrame chase_frame(const cars::bake::CarInfo& car, const drive::Vehicle& v, float aspect, bool front,
                          const drive::Ground* ground = nullptr, CameraPull* pull = nullptr, double dt = 0) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(car, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  const drive::Vec3 p = v.position();
  const drive::Vec3 fwd = drive::rotate(v.orientation(), {0, 0, 1});
  const float yaw = float(std::atan2(fwd.x, fwd.z));
  const V3 dir{std::sin(yaw), 0, std::cos(yaw)};
  const V3 base{float(p.x), float(p.y), float(p.z)};
  const V3 left{-dir.z, 0, dir.x};
  const V3 eye = front ? add(add(add(base, mul(dir, 4.0f)), mul(left, 3.0f)), V3{0, 0.9f, 0})
                       : add(add(base, mul(dir, -4.5f)), V3{0, 1.6f, 0});
  const V3 target = front ? add(base, V3{0, 0.5f, 0}) : add(add(base, mul(dir, 1.5f)), V3{0, 0.7f, 0});
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
                    norm(transform(world_to_model, {0, 1, 0}, 0)), aspect, false);
}

void wheel_poses(const drive::Vehicle& v, WheelPose poses[4]) {
  for (int i = 0; i < 4; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    poses[i].centre[0] = float(w.centre.x);
    poses[i].centre[1] = float(w.centre.y);
    poses[i].centre[2] = float(w.centre.z);
    poses[i].steer = float(w.steer);
    poses[i].spin = float(w.spin);
  }
}

const char* kDriveRuns[][2] = {
    {"rest", "0 0 0 0\n"},
    {"launch", "0 0 1 0\n"},
    {"slalom", "0 0 0.6 0\n2 0.8 0.5 0\n3 -0.8 0.5 0\n4 0.8 0.5 0\n5 -0.8 0.5 0\n6 0 0 0.6\n"},
    {"circle", "0 0 0 0\n1 0.5 0.35 0\n"},
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
    w.vertex_colours = mesh.colours;
    w.material = mesh.material;
    for (const auto& m : mesh.materials) w.materials.push_back({m.albedo, uint8_t(m.blend)});
  }
  return w;
}

// The world textures decoded so far, by texture record. Only the gathers'
// Prepare touches it, and gathers run one at a time.
struct WorldTextureLoader {
  std::set<uint32_t> done, failed;
  std::set<uint32_t> cutout;  // textures whose base level has alpha under one half
  size_t decoded = 0;
  // Decodes the textures `w` names that are new; a texture that does not
  // decode leaves its materials flat.
  void load(world::World& colorado, WorldTriangles& w) {
    for (auto& m : w.materials) {
      if (m.texture == kNoWorldTexture || done.count(m.texture)) continue;
      if (failed.count(m.texture)) {
        m.texture = kNoWorldTexture;
        continue;
      }
      fh1::WorldTexture t;
      std::string e;
      if (!colorado.read_texture(m.texture, t, &e)) {
        failed.insert(m.texture);
        m.texture = kNoWorldTexture;
        continue;
      }
      if (fh1::world_texture_has_cutout(t)) cutout.insert(m.texture);
      auto d = std::make_shared<WorldTextureData>();
      d->record = m.texture;
      d->srgb = t.format.gamma;
      char name[40];
      std::snprintf(name, sizeof name, "colorado:_0x%08x", colorado.textures()[m.texture].id);
      d->texture.name = name;
      d->texture.format = cars::bake::TextureFormat(uint32_t(t.blocks.format));
      d->texture.swizzle = t.blocks.swizzle;
      for (auto& l : t.blocks.levels) d->texture.levels.push_back({l.width, l.height, std::move(l.blocks)});
      done.insert(m.texture);
      w.textures.push_back(std::move(d));
      ++decoded;
    }
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
//            [--render-ground] [--flat-world] [--no-props] [--world-reference freeroam.json]]
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
// placed props out. --world-reference (headless)
// gathers around a world reference's camera and renders that one frame
// without the car at its matrix and size: <out> and, beside it, <out stem>_vs.png
// with the reference image on the left.
int run_drive(int argc, char** argv) {
  std::filesystem::path bake_dir, script_path, world_dir, cache_path;
  double near_x = world::kDumpCameraX, near_z = world::kDumpCameraZ, near_heading = world::kDumpCameraHeading;
  double cruise_kmh = 40, spawn[4] = {0, 0, 0, 0};
  bool near_given = false, have_spawn = false, render_ground = false, async_stream = false, flat_world = false, no_props = false;
  std::filesystem::path world_reference_path;
  std::string car, out, run = "slalom", view = "chase";
  uint32_t width = 960, height = 540;
  double seconds = 6, every = 1;
  bool headless = false;
  bool hud = false;  // headless: draw the HUD into the frames (windowed: always, H toggles)
  const bool validation = std::getenv("SHIFT_VALIDATION") != nullptr;
  // The engine sound (SPEC-ENGINE-SOUND), baked by soundbake.
  std::filesystem::path sound_dir = "out/sound", audio_out;
  bool sound = true;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--drive") continue;
    if (a == "--headless") headless = true;
    else if (a == "--bake") bake_dir = next();
    else if (a == "--car") car = next();
    else if (a == "--out") out = next();
    else if (a == "--run") run = next();
    else if (a == "--view") view = next();
    else if (a == "--script") script_path = next();
    else if (a == "--seconds") seconds = std::atof(next());
    else if (a == "--every") every = std::atof(next());
    else if (a == "--world") world_dir = next();
    else if (a == "--cache") cache_path = next();
    else if (a == "--speed") cruise_kmh = std::atof(next());
    else if (a == "--render-ground") render_ground = true;
    else if (a == "--flat-world") flat_world = true;
    else if (a == "--no-props") no_props = true;
    else if (a == "--world-reference") world_reference_path = next();
    else if (a == "--hud") hud = true;
    else if (a == "--sound") sound_dir = next();
    else if (a == "--no-sound") sound = false;
    else if (a == "--audio-out") audio_out = next();
    else if (a == "--async-stream") async_stream = true;
    else if (a == "--near") {
      near_x = std::atof(next());
      near_z = std::atof(next());
      near_given = true;
    } else if (a == "--spawn") {
      for (double& v : spawn) v = std::atof(next());
      have_spawn = true;
    }
    else if (a == "--size") {
      if (std::sscanf(next(), "%ux%u", &width, &height) != 2 || !width || !height) return 2;
    } else {
      std::fprintf(stderr, "carview --drive: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  if (bake_dir.empty() || car.empty()) {
    std::fprintf(stderr, "usage: carview --drive --bake <dir> --car <media> [--run name | --script file] "
                         "[--speed kmh] [--view chase|front] [--seconds N] [--every S] [--world <colorado dir> "
                         "[--cache file] [--near x z] [--spawn x y z heading] [--render-ground] [--flat-world] [--no-props] "
                         "[--world-reference file.json]] [--headless --out prefix.png [--hud] "
                         "[--audio-out file.wav]] [--sound dir | --no-sound] [--size WxH]\n");
    return 2;
  }
  if (view != "chase" && view != "front") {
    std::fprintf(stderr, "carview --drive: --view is chase or front\n");
    return 2;
  }
  bool front = view == "front";
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (sound ? SDL_INIT_AUDIO : 0))) {
      std::fprintf(stderr, "carview: SDL_Init: %s\n", SDL_GetError());
      return 1;
    }
    window = SDL_CreateWindow(("carview drive - " + car).c_str(), int(width), int(height),
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
    ShowroomOptions options;
    if (!showroom.load(bake_dir, car, options, error) || !renderer.init({window, validation, 4}, error) ||
        !showroom.upload(error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      status = 1;
    } else {
      const auto& info = showroom.car();
      std::vector<std::string> physics_notes;
      drive::Vehicle vehicle(drive::params_for_baked_car(info, bake_dir, &physics_notes));
      for (const auto& n : physics_notes) std::fprintf(stderr, "carview: physics: %s\n", n.c_str());
      // The engine sound: the baked loops, mixed at 48 kHz.
      constexpr int kAudioRate = 48000;
      audio::BakedSound baked_sound;
      std::unique_ptr<audio::EnginePlayer> engine_sound;
      if (sound && (!headless || !audio_out.empty())) {
        std::string sound_error;
        if (audio::load_baked_sound(sound_dir, car, baked_sound, &sound_error) &&
            !baked_sound.loaded.sound.groups.empty()) {
          engine_sound = std::make_unique<audio::EnginePlayer>(baked_sound.loaded.sound, baked_sound.table(),
                                                               kAudioRate, vehicle.params().idle_rpm,
                                                               vehicle.params().redline_rpm);
          engine_sound->master = 0.6;  // ours, by the mix's peaks
          std::printf("engine sound: %zu groups from %s\n", baked_sound.loaded.sound.groups.size(),
                      (sound_dir / car).string().c_str());
        } else {
          std::fprintf(stderr, "carview: no engine sound (%s)\n",
                       sound_error.empty() ? "no loops" : sound_error.c_str());
        }
      }
      // The sound's inputs from the engine: its throttle, and its torque as
      // a fraction of full throttle (driving) or of closed throttle (braking).
      auto sound_inputs = [&](bool bumper) {
        audio::EnginePlayer::Update u;
        const double rpm = vehicle.rpm(), thr = vehicle.engine_throttle();
        const double torque = vehicle.engine_torque(rpm, thr);
        const double full = vehicle.engine_torque(rpm, 1), closed = vehicle.engine_torque(rpm, 0);
        u.rpm = rpm;
        u.throttle = thr;
        u.pos_torque = torque > 0 && full > 0 ? torque / full : 0;
        u.neg_torque = torque < 0 && closed < 0 ? torque / closed : 0;
        u.view = bumper ? audio::View::kFront : audio::View::kFollow;
        return u;
      };
      std::vector<float> audio_track;  // headless --audio-out
      const drive::FlatGround flat;
      const drive::Ground* ground = &flat;
      world::World colorado;
      world::Collision collision;
      std::unique_ptr<world::Streamer> streamer;
      WorldTextureLoader texture_loader;
      world::RoadFollower follower(cruise_kmh / 3.6);
      drive::Vec3 start{};
      double start_heading = 0;
      const bool streamer_async = !headless || async_stream;
      // The streamer's staged scenery, swapped in.
      auto set_staged_world = [&]() {
        auto staged = std::any_cast<std::shared_ptr<Showroom::StagedWorld>>(std::move(streamer->prepared()));
        streamer->prepared().reset();
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
        if (cache_path.empty()) cache_path = std::filesystem::path("out") / "world" / "colorado.index";
        world::Spawn sp;
        std::string warning;
        const char* how = "";
        if (!colorado.open(world_dir, cache_path, &error)) {
          status = 1;
        } else if (!render_ground && !collision.open(world_dir, world_dir.parent_path().parent_path() / "physics.zip",
                                                     &error, &warning)) {
          status = 1;
        } else {
          if (!error.empty()) std::fprintf(stderr, "carview: note: %s\n", error.c_str());
          if (!warning.empty()) std::fprintf(stderr, "carview: note: %s", warning.c_str());
          error.clear();
          world::GatherOptions gather_options;
          gather_options.render_data = !flat_world;
          gather_options.props = !no_props;
          streamer = std::make_unique<world::Streamer>(colorado, gather_options, 200,
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
            WorldTriangles triangles = scenery(mesh, cx, cz);
            texture_loader.load(colorado, triangles);
            if (!showroom.stage_world(build_world(triangles), *staged, e)) staged.reset();
            if (staged) {
              staged->origin_x = cx;
              staged->origin_z = cz;
            }
            return std::any(staged);
          });
          const double sx = have_spawn ? spawn[0] : near_given || render_ground ? near_x : world::kSpawnX;
          const double sz = have_spawn ? spawn[2] : near_given || render_ground ? near_z : world::kSpawnZ;
          const double near[2] = {near_x, near_z};
          if (!streamer->update(sx, sz, nullptr, &error)) {
            status = 1;
          } else if (!world::choose_spawn(*streamer, have_spawn ? spawn : nullptr, near_given ? near : nullptr,
                                          near_given ? nullptr : &near_heading, sp, &how)) {
            error = "no road triangle near the search point";
            status = 1;
          }
        }
        if (status == 0) {
          std::printf("spawn %s: %.3f %.3f %.3f heading %.4f rad\n", how, sp.position[0], sp.position[1],
                      sp.position[2], sp.heading);
          start = {sp.position[0], sp.position[1], sp.position[2]};
          start_heading = sp.heading;
          vehicle.reset(start, start_heading);
          ground = &streamer->ground();
          follower.rebuild(streamer->mesh());
          follower.set_ground(&streamer->ground());
          if (!set_staged_world())
            status = 1;
          else
            showroom.set_background(0.42f, 0.55f, 0.72f);
        }
        if (status != 0) std::fprintf(stderr, "carview: %s\n", error.c_str());
      }
      // One fixed step, then the stream: gathers again (and swaps the
      // ground and the scenery) once the car is far from the last centre.
      auto step = [&](const drive::Input& in) {
        vehicle.step(in, *ground);
        if (!streamer) return true;
        // Headless runs put a car that is stuck or rolled back on the
        // nearest road (the windowed mode has R).
        if (headless && follower.stuck(vehicle, in)) {
          const drive::Vec3 at = vehicle.position();
          world::Spawn sp;
          if (world::recover(*streamer, vehicle, &sp))
            std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away\n", vehicle.time(), at.x,
                        at.y, at.z, sp.distance);
        }
        bool changed = false;
        const drive::Vec3 at = vehicle.position();
        if (!streamer->update(at.x, at.z, &changed, &error)) return false;
        if (changed) {
          ground = &streamer->ground();
          follower.rebuild(streamer->mesh());
          follower.set_ground(&streamer->ground());
          if (!set_staged_world()) return false;
          std::printf("t %.2f s: gathered again around %.1f %.1f (%zu triangles, %.2f s)\n", vehicle.time(),
                      streamer->centre_x(), streamer->centre_z(), streamer->mesh().triangles(),
                      streamer->last_gather_seconds());
        }
        bool swapped = false;
        showroom.poll_world(false, &swapped);
        if (swapped && streamer_async)
          std::printf("t %.2f s: scenery around %.1f %.1f uploaded\n", vehicle.time(), showroom.world_origin_x(),
                      showroom.world_origin_z());
        return true;
      };
      auto place_world = [&]() {
        if (!streamer) return;
        float m[16];
        scenery_transform(info, vehicle, showroom.world_origin_x(), showroom.world_origin_z(), m);
        showroom.set_world_transform(m);
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(m, kSunGame, sun);
        rotate_direction(m, up_game, up);
        showroom.set_world_light(sun, up, 1.0f);
      };
      WheelPose poses[4];
      CameraPull camera_pull;
      // The HUD panel in the bottom-right corner, 16 px in (scaled).
      auto set_hud = [&](int height) {
        HudState st;
        st.kmh = vehicle.speed() * 3.6;
        st.gear = vehicle.gear();
        st.rpm = vehicle.rpm();
        st.redline_rpm = vehicle.params().redline_rpm;
        st.abs = vehicle.abs_active();
        const int scale = hud_scale(height);
        HudImage img = draw_hud(st, scale);
        const int margin = -8 * scale;
        renderer.set_overlay(std::move(img.rgba), img.width, img.height, margin, margin);
      };
      if (status == 0 && streamer) {
        const auto& st = streamer->stats();
        std::printf("scenery: %zu triangles, %zu materials, %zu textures decoded (%zu failed), %zu models with the "
                    "default UV scale, %zu submeshes without a material\n",
                    streamer->mesh().triangles(), streamer->mesh().materials.size(), texture_loader.decoded,
                    texture_loader.failed.size(), st.default_uv_models.size(), st.no_material);
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
            std::printf("  material %u %s texture %u blend %d cut-out %d: %zu triangles, UV per metre %.4g\n", m,
                        mm.effect.c_str(), mm.albedo, int(mm.blend), int(texture_loader.cutout.count(mm.albedo)),
                        v.size(), v[v.size() / 2]);
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
        ShowroomFrame frame{};
        std::memcpy(frame.view_projection, world_reference.clip_from_world, sizeof frame.view_projection);
        for (int c = 0; c < 4; ++c) frame.view_projection[c * 4 + 1] = -frame.view_projection[c * 4 + 1];
        std::memcpy(frame.transparent_view_projection, frame.view_projection, sizeof frame.view_projection);
        for (int k = 0; k < 3; ++k) frame.eye[k] = world_reference.camera_position[k];
        frame.options[0] = 1;
        showroom.set_draw_car(false);
        showroom.set_frame(frame);
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
      } else if (headless) {
        const std::filesystem::path prefix(out);
        const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
        const int stride = std::max(1, int(std::lround(every / drive::Vehicle::kStep)));
        double longest_step = 0;  // wall time of one step with its streaming, s
        for (int s = 0; s <= steps && status == 0; ++s) {
          const auto t0 = std::chrono::steady_clock::now();
          const bool ok =
              s == 0 || step(cruise ? follower.input(vehicle) : drive::script_input(script, vehicle.time()));
          if (engine_sound && s > 0) {
            // One physics step of sound, the inputs set once per step.
            engine_sound->set(sound_inputs(front));
            const int frames = int(std::lround(kAudioRate * drive::Vehicle::kStep));
            const size_t at = audio_track.size();
            audio_track.resize(at + 2 * size_t(frames));
            engine_sound->render(audio_track.data() + at, frames);
          }
          longest_step =
              std::max(longest_step, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
          if (!ok) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          if (s % stride != 0) continue;
          wheel_poses(vehicle, poses);
          showroom.set_wheel_poses(poses);
          place_world();
          showroom.set_frame(chase_frame(info, vehicle, float(width) / float(height), front, ground, &camera_pull,
                                         every));
          if (hud) set_hud(int(height));
          std::vector<uint8_t> rgba;
          if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          char name[32];
          std::snprintf(name, sizeof name, "_%05d.png", int(std::lround(vehicle.time() * 100)));
          const std::string path = (prefix.parent_path() / (prefix.stem().string() + name)).string();
          if (!write_png(path, width, height, 4, rgba.data())) {
            std::fprintf(stderr, "carview: cannot write %s\n", path.c_str());
            status = 1;
          }
          const auto& wl = vehicle.wheels()[0];
          const drive::Vec3 at = vehicle.position();
          std::printf("t %.2f s: %.1f km/h, gear %d, %.0f rpm, LF steer %.3f rad compression %.3f m, at %.1f %.1f "
                      "%.1f -> %s\n",
                      vehicle.time(), vehicle.speed() * 3.6, vehicle.gear(), vehicle.rpm(), wl.steer, wl.compression,
                      at.x, at.y, at.z, path.c_str());
        }
        if (streamer)
          std::printf("longest step with its streaming: %.1f ms (%s gathers)\n", longest_step * 1000,
                      async_stream ? "background" : "synchronous");
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
        uint64_t last = SDL_GetTicksNS(), title_at = 0;
        double accumulator = 0;
        bool running = true;
        std::printf("controls: W/Up throttle, S/Down brake (held at a stop: reverse), A/D or Left/Right steer, "
                    "Space handbrake, R put back on the road, V chase/front view, H hide/show the HUD, Esc quit.\n"
                    "gamepad: right trigger throttle, left trigger brake, left stick steer, A handbrake, Start put "
                    "back on the road, Back view.\n");
        bool show_hud = true;
        // The engine sound's stream: about 60 ms queued ahead.
        SDL_AudioStream* audio_stream = nullptr;
        if (engine_sound) {
          const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
          audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
          if (audio_stream) SDL_ResumeAudioStreamDevice(audio_stream);
          else std::fprintf(stderr, "carview: no audio device: %s\n", SDL_GetError());
        }
        std::vector<float> audio_block;
        // R or Start: back on the nearest road (with a world), else to the start.
        auto put_back = [&]() {
          world::Spawn where;
          if (streamer && world::recover(*streamer, vehicle, &where))
            std::printf("t %.2f s: put back on the road %.1f m away\n", vehicle.time(), where.distance);
          else
            vehicle.reset(start, start_heading);
          camera_pull = CameraPull{};
        };
        while (running) {
          SDL_Event e;
          while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
              if (e.key.key == SDLK_ESCAPE) running = false;
              if (e.key.key == SDLK_R) put_back();
              if (e.key.key == SDLK_V) front = !front;
              if (e.key.key == SDLK_H) {
                show_hud = !show_hud;
                if (!show_hud) renderer.set_overlay({}, 0, 0, 0, 0);
              }
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) put_back();
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) front = !front;
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
              if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) {
                pads.push_back(pad);
                std::printf("gamepad: %s\n", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "(unnamed)");
              }
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
              for (size_t k = 0; k < pads.size(); ++k)
                if (SDL_GetGamepadID(pads[k]) == e.gdevice.which) {
                  SDL_CloseGamepad(pads[k]);
                  pads.erase(pads.begin() + long(k));
                  break;
                }
          }
          // Keyboard, then the first gamepad with any input.
          const bool* keys = SDL_GetKeyboardState(nullptr);
          drive::Input in;
          in.steer = (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D] ? 1.0 : 0.0) -
                     (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A] ? 1.0 : 0.0);
          in.throttle = keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W] ? 1.0 : 0.0;
          in.brake = keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S] ? 1.0 : 0.0;
          in.handbrake = keys[SDL_SCANCODE_SPACE];
          for (SDL_Gamepad* pad : pads) {
            auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
            const double sx = axis(SDL_GAMEPAD_AXIS_LEFTX);
            const double steer = std::fabs(sx) < 0.12 ? 0.0 : sx;
            const double throttle = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), brake = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            if (steer != 0 || throttle > 0.02 || brake > 0.02 || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) {
              in.steer = steer;
              in.throttle = std::max(0.0, throttle);
              in.brake = std::max(0.0, brake);
              in.handbrake = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
              break;
            }
          }
          const uint64_t now = SDL_GetTicksNS();
          const double frame_dt = std::min(0.25, double(now - last) * 1e-9);
          accumulator += frame_dt;
          last = now;
          while (accumulator >= drive::Vehicle::kStep) {
            if (!step(cruise ? follower.input(vehicle) : in)) {
              running = false;
              status = 1;
              std::fprintf(stderr, "carview: %s\n", error.c_str());
              break;
            }
            accumulator -= drive::Vehicle::kStep;
          }
          if (now - title_at > 100000000) {  // the speedometer, ten times a second
            title_at = now;
            char title[160], gear[8];
            if (vehicle.gear() < 0) std::snprintf(gear, sizeof gear, "R");
            else if (vehicle.gear() == 0) std::snprintf(gear, sizeof gear, "N");
            else std::snprintf(gear, sizeof gear, "%d", vehicle.gear());
            std::snprintf(title, sizeof title, "carview drive - %s - %3.0f km/h  gear %s  %5.0f rpm%s", car.c_str(),
                          std::fabs(vehicle.speed()) * 3.6, gear, vehicle.rpm(), vehicle.abs_active() ? "  ABS" : "");
            SDL_SetWindowTitle(window, title);
          }
          if (audio_stream) {
            engine_sound->set(sound_inputs(front));
            const int queued = SDL_GetAudioStreamQueued(audio_stream) / int(2 * sizeof(float));
            const int frames = int(0.06 * kAudioRate) - queued;
            if (frames > 0) {
              audio_block.resize(2 * size_t(frames));
              engine_sound->render(audio_block.data(), frames);
              SDL_PutAudioStreamData(audio_stream, audio_block.data(), int(audio_block.size() * sizeof(float)));
            }
          }
          int w = 0, h = 0;
          SDL_GetWindowSizeInPixels(window, &w, &h);
          if (show_hud) set_hud(h);
          wheel_poses(vehicle, poses);
          showroom.set_wheel_poses(poses);
          place_world();
          showroom.set_frame(chase_frame(info, vehicle, h > 0 ? float(w) / float(h) : 1.0f, front, ground,
                                         &camera_pull, frame_dt));
          if (!renderer.draw_frame(FrameConstants{}, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
        }
        for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
        if (audio_stream) SDL_DestroyAudioStream(audio_stream);
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

}  // namespace carview
