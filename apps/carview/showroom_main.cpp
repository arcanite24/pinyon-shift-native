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
#include "audio/xml.h"
#include "hud.h"
#include "process_time.h"
#include "reference.h"
#include "renderer.h"
#include "showroom.h"
#include "traffic_view.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "drive/save.h"
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
  const float near_z = 0.25f, far_z = 16000.0f;  // the far ring reaches several km
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
                          const drive::Ground* ground = nullptr, CameraPull* pull = nullptr, double dt = 0) {
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
    for (const auto& m : mesh.materials)
      w.materials.push_back({m.albedo, uint8_t(m.blend), m.far, uint8_t(m.layers), m.layer_b, m.splat, m.tiling});
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
    for (auto& m : w.materials)
      for (uint32_t* slot : {&m.texture, &m.layer_b, &m.splat}) load_one(colorado, w, *slot);
  }
  void load_one(world::World& colorado, WorldTriangles& w, uint32_t& record) {
    {
      if (record == kNoWorldTexture || done.count(record)) return;
      if (failed.count(record)) {
        record = kNoWorldTexture;
        return;
      }
      fh1::WorldTexture t;
      std::string e;
      if (!colorado.read_texture(record, t, &e)) {
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
      done.insert(record);
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
// The scene's scale into the filmic curve (ours: the track's filmicTone
// exposure, 0.31 on Colorado, applies to a scene the game lights more
// brightly than ours; set so the free-roam reference frame's mean is met).
constexpr double kPostExposure = 0.8;

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

void drive_usage() {
  std::fprintf(stderr,
               "usage: carview [options]   (the drive; the default mode)\n"
               "  where:   --play <dir> (play folder: play.txt, bake/, game) | --bake <dir> --world <colorado dir>\n"
               "           --car <media> --hour h --spawn x y z heading --near x z --cache file\n"
               "           --save file | --no-save\n"
               "  look:    --size WxH --msaa N --preset low|high --no-post --bloom | --no-bloom --exposure x\n"
               "           --shadows off|low|high --view chase|front --hud\n"
               "  world:   --render-ground --flat-world --no-props --no-far --no-trees --async-stream\n"
               "  traffic: --traffic | --no-traffic --traffic-seed N\n"
               "  sound:   --sound <dir> | --no-sound\n"
               "  scripted and headless: --run name | --script file --speed kmh --seconds N --every S\n"
               "           --headless --out prefix.png --audio-out file.wav --bench --world-reference file.json\n"
               "           --menu-png file.png\n"
               "  other modes: carview --bake <dir> --car <media> (showroom), carview --game-root <dir> (disc viewer),\n"
               "           carview --world ... (world viewer), carview --xds-png ...\n");
}

int run_drive(int argc, char** argv) {
  // The post chain (on unless --no-post); --exposure overrides ours,
  // --bloom / --no-bloom the preset's choice.
  bool post_on = true;
  int bloom_flag = -1;
  double exposure_given = 0;
  bool low_preset = false;
  // The sun shadow (ours): --shadows off, low (1024 texels over 80 m, one
  // filtered compare) or high (2048 over 120 m, four); the preset's by
  // default (low: low, otherwise high).
  std::string shadows_flag;
  std::filesystem::path bake_dir, script_path, world_dir, cache_path;
  double near_x = world::kDumpCameraX, near_z = world::kDumpCameraZ, near_heading = world::kDumpCameraHeading;
  double cruise_kmh = 40, spawn[4] = {0, 0, 0, 0};
  bool near_given = false, have_spawn = false, render_ground = false, async_stream = false, flat_world = false, no_props = false,
       no_far = false, no_trees = false;
  bool bench = false;     // headless: render every frame, timed, no images
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
  bool no_save = false;
  const bool validation = std::getenv("SHIFT_VALIDATION") != nullptr;
  // The engine sound (SPEC-ENGINE-SOUND), baked by soundbake.
  std::filesystem::path sound_dir = "out/sound", audio_out;
  bool sound = true, sound_given = false;
  // The play folder (tools/setup.py; README, Play the drive).
  std::filesystem::path play_dir;
  // Free-roam traffic (SPEC-TRAFFIC): on in the window with the world, off
  // headless unless --traffic; --hour sets the time of day it follows (the
  // world's lighting does not change with it), T / Shift+T in the window.
  int traffic_flag = -1;  // -1: the default
  double hour = 12;
  uint64_t traffic_seed = 1;
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
    else if (a == "--no-far") no_far = true;
    else if (a == "--no-trees") no_trees = true;
    else if (a == "--bench") bench = headless = async_stream = true;  // streams like the window
    else if (a == "--msaa") msaa = uint32_t(std::atoi(next()));
    else if (a == "--preset") {
      // low (ours): no MSAA, cut-outs alpha-tested, no bloom; high: the
      // default 4x, bloom.
      const std::string p = next();
      msaa = p == "low" ? 1 : 4;
      low_preset = p == "low";
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
    else if (a == "--exposure") exposure_given = std::atof(next());
    else if (a == "--world-reference") world_reference_path = next();
    else if (a == "--hud") hud = true;
    else if (a == "--sound") sound_dir = next(), sound_given = true;
    else if (a == "--play") play_dir = next();
    else if (a == "--no-sound") sound = false;
    else if (a == "--traffic") traffic_flag = 1;
    else if (a == "--no-traffic") traffic_flag = 0;
    else if (a == "--hour") hour = std::fmod(std::fmod(std::atof(next()), 24.0) + 24.0, 24.0);
    else if (a == "--traffic-seed") traffic_seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--audio-out") audio_out = next();
    else if (a == "--async-stream") async_stream = true;
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
    } else if (a == "--menu-png") {
      menu_png = next();
    }
    else if (a == "--size") {
      if (std::sscanf(next(), "%ux%u", &width, &height) != 2 || !width || !height) return 2;
      size_given = true;
    } else if (a == "--help" || a == "-h") {
      drive_usage();
      return 0;
    } else {
      std::fprintf(stderr, "carview: unknown option %s\n", a.c_str());
      drive_usage();
      return 2;
    }
  }
  if (!menu_png.empty()) {
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
    const HudImage img = draw_hud(st, 3);
    if (!write_png(menu_png.string(), img.width, img.height, 4, img.rgba.data())) return 1;
    std::printf("menu: %ux%u to %s\n", img.width, img.height, menu_png.string().c_str());
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
  if (have_saved && !have_spawn && (!car_given || lower_ascii(saved.car) == lower_ascii(car))) {
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
  // drive with it where the last car stood (window and SDL kept).
  std::vector<std::string> baked_cars;
  {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(bake_dir / "cars", ec))
      if (e.path().extension() == ".shiftbake") baked_cars.push_back(e.path().stem().string());
    std::sort(baked_cars.begin(), baked_cars.end());
  }
  std::string next_car;
  for (bool again = true; again;) {
  again = false;
  {
    Renderer renderer;
    Showroom showroom(renderer);
    // SHIFT_NO_CULL=1: every world draw, for comparison with the culled frame.
    if (const char* e = std::getenv("SHIFT_NO_CULL"); e && *e == '1') showroom.set_world_culling(false);
    ShowroomOptions options;
    Renderer::Options render_options{window, validation, msaa};
    if (post_on) {
      PostSettings& post = render_options.post;
      post.enabled = true;
      if (!world_dir.empty()) read_post_settings(world_dir, post);
      post.exposure = float(exposure_given > 0 ? exposure_given : kPostExposure);
      post.bloom = bloom_flag >= 0 ? bloom_flag == 1 : !low_preset;
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
    if (!showroom.load(bake_dir, car, options, error) || !renderer.init(render_options, error) ||
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
      // What the streaming worker prepares with each gather: the staged
      // scenery and the road follower's points.
      struct Prepared {
        std::shared_ptr<Showroom::StagedWorld> staged;
        world::RoadFollower::Points road;
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
          if (!flat_world && !no_far) gather_options.far_radius = 3000;
          if (!flat_world && !no_trees) gather_options.tree_radius = kTreeRadius;
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
            auto prepared = std::make_shared<Prepared>();
            prepared->staged = std::move(staged);
            prepared->road = world::RoadFollower::road_points(mesh);
            return std::any(prepared);
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
      }
      std::unique_ptr<TrafficView> traffic;
      if (status == 0 && streamer && (traffic_flag == 1 || (traffic_flag < 0 && !headless))) {
        traffic = std::make_unique<TrafficView>(renderer, showroom, bake_dir, headless && !async_stream);
        const auto game_root = world_dir.parent_path().parent_path().parent_path();
        if (!traffic->open(game_root, &ground, hour, traffic_seed, error)) {
          std::fprintf(stderr, "carview: note: no traffic: %s\n", error.c_str());
          error.clear();
          traffic.reset();
        }
      }
      // One fixed step, then the stream: gathers again (and swaps the
      // ground and the scenery) once the car is far from the last centre.
      auto step = [&](const drive::Input& in) {
        vehicle.step(in, *ground);
        if (traffic) traffic->step(vehicle);
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
        return true;
      };
      auto place_world = [&]() {
        if (!streamer) return;
        float m[16];
        scenery_transform(info, vehicle, showroom.world_origin_x(), showroom.world_origin_z(), m);
        showroom.set_world_transform(m);
        if (traffic) traffic->place(info, vehicle);
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(m, kSunGame, sun);
        rotate_direction(m, up_game, up);
        showroom.set_world_light(sun, up, 1.0f);
        if (have_shadow_light) {
          float towards[3];
          rotate_direction(m, shadow_light_game, towards);
          showroom.set_shadow_direction(towards);
        }
      };
      // Writes where the car stands (drive/save.h); the start is the last spawn.
      auto save_now = [&](const char* why) {
        if (save_path.empty() || status != 0) return;
        drive::DriveSave s;
        s.car = car;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
        s.position = {at.x, at.y, at.z};
        s.heading = std::atan2(fwd.x, fwd.z);
        s.has_spawn = streamer != nullptr;
        s.spawn = {start.x, start.y, start.z};
        s.spawn_heading = start_heading;
        std::string e;
        if (!drive::write_save(save_path, s, &e)) std::fprintf(stderr, "carview: %s\n", e.c_str());
        else if (headless) std::printf("save (%s): %s at %.1f %.1f %.1f\n", why, save_path.string().c_str(), at.x, at.y, at.z);
        saved = s;
        have_saved = true;
      };
      WheelPose poses[4];
      CameraPull camera_pull;
      // The car's name and the keys, over the HUD for the first seconds.
      constexpr double kIntroSeconds = 6;
      // (The display name is a string-table key: the media name is shown.)
      const std::string intro = car + " - [ ] pick a car  R back on the road  T time of day  Esc menu";
      // A line for a few seconds after the time of day changes.
      std::string notice;
      double notice_until = -1;
      // The HUD panel in the bottom-right corner, 16 px in (scaled).
      auto set_hud = [&](int height, const std::string& banner, const PauseMenu* menu = nullptr) {
        HudState st;
        st.banner = banner;
        if (menu) menu->fill(st);
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
        if (st.far_models)
          std::printf("far ring: %zu models, %zu triangles\n", st.far_models, st.far_triangles);
        if (st.trees || st.trees_untextured)
          std::printf("tree sprites: %zu (%zu without an atlas file)\n", st.trees, st.trees_untextured);
        std::printf("props: %zu placed, %zu unresolved; left out: %zu off the free-roam route, %zu markers, %zu tree "
                    "sprites off the route\n",
                    st.props, st.prop_unresolved, st.props_off_route, st.props_markers, st.trees_off_route);
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
      } else if (bench) {
        // The benchmark (M2 performance): the cruise along the free-roam
        // road from the spawn, a frame every second step (60 Hz of game
        // time), each rendered and timed without reading it back. Per
        // frame: the CPU time of the steps since the last frame (with the
        // streaming's hand-over), of posing the scene and of recording the
        // commands; the GPU time between timestamps. Process CPU time over
        // the whole run counts every thread (the gather worker, the
        // driver's).
        const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
        std::vector<double> cpu_ms, gpu_ms, sim_ms, record_ms, pose_ms;
        // The world's draws per frame: recorded, culled, indices drawn.
        double draws_sum = 0, culled_sum = 0, indices_sum = 0, shadow_sum = 0, shadow_cars_sum = 0;
        std::vector<std::string> stage_names;
        std::vector<std::vector<double>> stage_ms;  // per stage, per frame
        double sim = 0;
        const double cpu0 = process_cpu_seconds();
        const auto wall0 = std::chrono::steady_clock::now();
        for (int s = 0; s <= steps && status == 0; ++s) {
          const auto t0 = std::chrono::steady_clock::now();
          if (s > 0 && !step(cruise ? follower.input(vehicle) : drive::script_input(script, vehicle.time()))) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          sim += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          if (s % 2 != 0) continue;
          const auto p0 = std::chrono::steady_clock::now();
          wheel_poses(vehicle, poses);
          showroom.set_wheel_poses(poses);
          place_world();
          showroom.set_frame(chase_frame(info, vehicle, float(width) / float(height), front, ground, &camera_pull,
                                         2 * drive::Vehicle::kStep));
          const double pose = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();
          double rec = 0, gpu = 0;
          if (!renderer.render_timed(FrameConstants{}, width, height, rec, gpu, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
          sim_ms.push_back(sim);
          pose_ms.push_back(pose);
          record_ms.push_back(rec);
          {
            const auto ws = showroom.world_draw_stats();
            draws_sum += double(ws.drawn), culled_sum += double(ws.culled), indices_sum += double(ws.indices);
            shadow_sum += double(showroom.shadow_draw_count());
            shadow_cars_sum += double(showroom.shadow_car_count());
          }
          for (const auto& [name, ms] : renderer.stage_ms()) {
            size_t k = 0;
            while (k < stage_names.size() && stage_names[k] != name) ++k;
            if (k == stage_names.size()) {
              stage_names.push_back(name);
              stage_ms.emplace_back();
            }
            stage_ms[k].push_back(ms);
          }
          cpu_ms.push_back(sim + pose + rec);
          gpu_ms.push_back(gpu);
          sim = 0;
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        const double cpu = process_cpu_seconds() - cpu0;
        auto pct = [](std::vector<double> v, double q) {
          if (v.empty()) return 0.0;
          std::sort(v.begin(), v.end());
          return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
        };
        const size_t n = cpu_ms.size();
        std::printf("bench: %s, %ux%u, MSAA %u, %zu frames over %.1f s of game time (%.1f s wall)\n",
                    renderer.device_name().c_str(), width, height, renderer.sample_count(), n, seconds, wall);
        std::printf("bench: frame CPU ms p50 %.2f p95 %.2f p99 %.2f max %.2f (steps p50 %.2f max %.2f; recording "
                    "p50 %.2f)\n",
                    pct(cpu_ms, 0.5), pct(cpu_ms, 0.95), pct(cpu_ms, 0.99), pct(cpu_ms, 1.0), pct(sim_ms, 0.5),
                    pct(sim_ms, 1.0), pct(record_ms, 0.5));
        {
          // The slowest frames, by game time (a hitch shows which event).
          std::vector<size_t> order(n);
          for (size_t k = 0; k < n; ++k) order[k] = k;
          std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cpu_ms[a] > cpu_ms[b]; });
          std::printf("bench: slowest frames (CPU ms at game time: steps + posing + recording):");
          for (size_t k = 0; k < std::min<size_t>(5, n); ++k) {
            const size_t f = order[k];
            std::printf(" %.2f at %.2f s (%.2f + %.2f + %.2f);", cpu_ms[f], double(f) * 2 * drive::Vehicle::kStep,
                        sim_ms[f], pose_ms[f], record_ms[f]);
          }
          std::printf("\n");
        }
        if (n && gpu_ms[0] >= 0)
          std::printf("bench: frame GPU ms p50 %.2f p95 %.2f p99 %.2f max %.2f\n", pct(gpu_ms, 0.5),
                      pct(gpu_ms, 0.95), pct(gpu_ms, 0.99), pct(gpu_ms, 1.0));
        else
          std::printf("bench: no GPU timestamps on this queue\n");
        if (!stage_names.empty()) {
          std::printf("bench: GPU by stage, p50 ms:");
          for (size_t k = 0; k < stage_names.size(); ++k)
            std::printf(" %s %.2f;", stage_names[k].c_str(), pct(stage_ms[k], 0.5));
          std::printf("\n");
        }
        if (n)
          std::printf("bench: world draws per frame %.0f drawn, %.0f culled, %.0fk of %.0fk triangles drawn\n",
                      draws_sum / double(n), culled_sum / double(n), indices_sum / double(n) / 3000.0,
                      double(showroom.world_triangle_count()) / 1000.0);
        if (n && shadow_sum > 0)
          std::printf("bench: shadow casters per frame %.0f world draws, %.2f traffic cars\n", shadow_sum / double(n),
                      shadow_cars_sum / double(n));
        std::printf("bench: process CPU %.2f s over %.2f s wall: %.2f ms per frame, %.0f%% of one core\n", cpu, wall,
                    n ? cpu * 1000 / double(n) : 0.0, wall > 0 ? cpu / wall * 100 : 0.0);
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
          if (hud) set_hud(int(height), vehicle.time() < kIntroSeconds ? intro : std::string());
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
                    "Space handbrake, R put back on the road, V chase/front view, C (held) look back, H hide/show "
                    "the HUD, Esc pause menu.\n"
                    "gamepad: right trigger throttle, left trigger brake, left stick steer, A handbrake, right stick "
                    "look around, Y (held) look back, Start pause menu, Back view.\n"
                    "car picker: [ and ] (LB and RB) choose among the %zu baked cars, Enter (X) drives it.\n",
                    baked_cars.size());
        // The picker's choice, shown in the title until it is taken.
        size_t pick = 0;
        bool picking = false;
        for (size_t k = 0; k < baked_cars.size(); ++k)
          if (baked_cars[k] == lower_ascii(car)) pick = k;
        auto browse = [&](int by) {
          if (baked_cars.empty()) return;
          pick = (pick + baked_cars.size() + size_t(by + int(baked_cars.size()))) % baked_cars.size();
          picking = true;
          title_at = 0;  // show it now
        };
        auto take_pick = [&]() {
          if (!picking || baked_cars.empty()) return;
          // Restart with that car where this one stands.
          const drive::Vec3 at = vehicle.position();
          const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
          spawn[0] = at.x;
          spawn[1] = at.y;
          spawn[2] = at.z;
          spawn[3] = std::atan2(fwd.x, fwd.z);
          have_spawn = true;
          next_car = baked_cars[pick];
          running = false;
        };
        bool show_hud = true;
        PauseMenu menu;
        double next_save = 30;  // s of drive time: the save is also written every 30 s
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
        // The chosen item: resume, the picker, back on the road, or quit.
        auto choose = [&]() {
          const int item = menu.selected;
          menu.open = false;
          if (item == PauseMenu::kChangeCar) browse(0);
          if (item == PauseMenu::kReset) put_back();
          if (item == PauseMenu::kQuit) running = false;
        };
        auto open_menu = [&]() {
          menu.show();
          picking = false;
          save_now("pause");
        };
        while (running) {
          SDL_Event e;
          while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            // The pause menu takes the keys and buttons while open.
            if (menu.open) {
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) menu.open = false;
                if (k == SDLK_UP || k == SDLK_W) menu.move(-1);
                if (k == SDLK_DOWN || k == SDLK_S) menu.move(1);
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) choose();
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) menu.open = false;
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) menu.move(-1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu.move(1);
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) choose();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
              if (e.key.key == SDLK_ESCAPE) {
                if (picking) picking = false;
                else open_menu();
              }
              if (e.key.key == SDLK_R) put_back();
              if (e.key.key == SDLK_V) front = !front;
              if (e.key.key == SDLK_LEFTBRACKET) browse(-1);
              if (e.key.key == SDLK_RIGHTBRACKET) browse(1);
              if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) take_pick();
              if (e.key.key == SDLK_T && traffic) {
                const bool back = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                traffic->set_hour(std::fmod(traffic->hour() + (back ? 23 : 1), 24.0));
                char line_buf[64];
                std::snprintf(line_buf, sizeof line_buf, "traffic at %02d:00", int(traffic->hour()));
                notice = line_buf;
                notice_until = vehicle.time() + 3;
              }
              if (e.key.key == SDLK_H) {
                show_hud = !show_hud;
                if (!show_hud) renderer.set_overlay({}, 0, 0, 0, 0);
              }
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) open_menu();
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) front = !front;
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) browse(-1);
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) browse(1);
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) take_pick();
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
          // Look around: the right stick turns the chase camera up to half a
          // turn, Y (or C held) looks back.
          camera_pull.look_target = keys[SDL_SCANCODE_C] ? 3.14159265358979 : 0.0;
          for (SDL_Gamepad* pad : pads) {
            auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
            const double steer = deadzone(axis(SDL_GAMEPAD_AXIS_LEFTX), 0.10, 0.95);
            const double throttle = deadzone(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.03, 0.97);
            const double brake = deadzone(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.03, 0.97);
            const double look = deadzone(axis(SDL_GAMEPAD_AXIS_RIGHTX), 0.20, 0.95);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH)) camera_pull.look_target = 3.14159265358979;
            else if (look != 0) camera_pull.look_target = -look * 3.14159265358979;
            if (steer != 0 || throttle > 0 || brake > 0 || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) {
              in.steer = steer;
              in.throttle = throttle;
              in.brake = brake;
              in.handbrake = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
              break;
            }
          }
          const uint64_t now = SDL_GetTicksNS();
          const double frame_dt = std::min(0.25, double(now - last) * 1e-9);
          // Paused: the car holds still (no steps, no time to catch up).
          if (!menu.open) accumulator += frame_dt;
          last = now;
          if (vehicle.time() >= next_save) {
            save_now("timer");
            next_save = vehicle.time() + 30;
          }
          while (!menu.open && accumulator >= drive::Vehicle::kStep) {
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
            char title[320], gear[8];
            if (vehicle.gear() < 0) std::snprintf(gear, sizeof gear, "R");
            else if (vehicle.gear() == 0) std::snprintf(gear, sizeof gear, "N");
            else std::snprintf(gear, sizeof gear, "%d", vehicle.gear());
            std::snprintf(title, sizeof title, "carview drive - %s - %3.0f km/h  gear %s  %5.0f rpm%s", car.c_str(),
                          std::fabs(vehicle.speed()) * 3.6, gear, vehicle.rpm(), vehicle.abs_active() ? "  ABS" : "");
            if (picking) {
              const size_t n = std::strlen(title);
              std::snprintf(title + n, sizeof title - n, "   |   pick %zu/%zu: %s (Enter or X to drive it)", pick + 1,
                            baked_cars.size(), baked_cars[pick].c_str());
            }
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
          if (show_hud) {
            char line[160] = "";
            if (picking)
              std::snprintf(line, sizeof line, "car %zu/%zu: %s - Enter drives it", pick + 1, baked_cars.size(),
                            baked_cars[pick].c_str());
            set_hud(h,
                    picking                          ? std::string(line)
                    : vehicle.time() < notice_until  ? notice
                    : vehicle.time() < kIntroSeconds ? intro
                                                     : std::string(),
                    &menu);
          }
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
      save_now("end");
      if (traffic) std::printf("%s\n", traffic->summary().c_str());
      if (validation) std::printf("validation messages: %u\n", renderer.validation_messages());
      if (status == 0 && renderer.validation_messages()) status = 3;
    }
  }
  if (status == 0 && !next_car.empty()) {
    car = next_car;
    next_car.clear();
    again = true;
    SDL_SetWindowTitle(window, ("carview drive - loading " + car).c_str());
  }
  }
  if (window) {
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
  return status;
}

}  // namespace carview
