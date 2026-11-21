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
#include <future>
#include <map>
#include <random>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include "audio/bake.h"
#include "audio/radio.h"
#include "audio/xml.h"
#include "hud.h"
#include "process_time.h"
#include "reference.h"
#include "renderer.h"
#include "showroom.h"
#include "traffic_view.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/cameras.h"
#include "drive/ai_driver.h"
#include "drive/contact.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "drive/save.h"
#include "drive/vehicle.h"
#include "world/road_follow.h"
#include "world/stream.h"
#include "world/world.h"
#include "contact_log.h"
#include "safe_reset.h"
#include "race_view.h"
#include "map_view.h"
#include "fh1data/archive.h"
#include "fh1data/xds.h"
#include "freeroam/collectibles.h"
#include "profile/drive_save.h"
#include "garage/garage.h"
#include "upgrade_shop.h"
#include "autoshow.h"
#include "cars/mods.h"
#include "career_page.h"
#include "settings.h"

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
ShowroomFrame look_frame(V3 eye, V3 target, V3 up, float aspect, bool livery, bool reverse_z = false,
                         float fov_degrees = 55.0f, float near_clip = 0.25f) {
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

// SPEC-CAMERAS' rig for the drive: the tuning from camera.zip (v4's layers
// when installed), the car's CameraOverrides row and MAXData box from the
// player's files, the CoM and steering wheel from the car in use. False
// (with a note) when something is missing: the drive keeps the old chase.
bool load_camera_rig(const std::filesystem::path& game_root, const std::filesystem::path& bake_dir,
                     const cars::bake::CarInfo& info, const drive::Vehicle& v, drive::DriveCamera& rig,
                     std::string& note) {
  cars::Install install;
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  std::string ini, xml;
  drive::CameraTuning tuning;
  if (!cars::discover_install(game_root, state ? state : "", "", install, &note) ||
      !cars::load_camera_files(install, ini, xml, &note) || !drive::parse_camera_settings(ini, tuning, &note) ||
      !drive::parse_camera_physics(xml, tuning, &note))
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

// The rig's frame: the car's pose and motion in, the view out (in the
// car's model frame, where the showroom draws).
ShowroomFrame rig_frame(drive::DriveCamera& rig, const cars::bake::CarInfo& info, const drive::Vehicle& v,
                        const drive::Ground* ground, double dt, float aspect, bool reverse_z, bool* interior,
                        double* com_distance = nullptr) {
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
  {
    static drive::Vec3 last_vel{};
    static bool have_last = false;
    if (have_last && dt > 1e-4 && dt < 0.25) {
      const drive::Vec3 a{(vel.x - last_vel.x) / dt, (vel.y - last_vel.y) / dt, (vel.z - last_vel.z) / dt};
      const drive::Vec3 right = drive::rotate(v.orientation(), {1, 0, 0});
      in.g_longitudinal = (a.x * fwd.x + a.y * fwd.y + a.z * fwd.z) / 9.81;
      in.g_lateral = (a.x * right.x + a.y * right.y + a.z * right.z) / 9.81;
    }
    last_vel = vel;
    have_last = true;
  }
  int down = 0;
  const double c = std::cos(info.pitch), s = std::sin(info.pitch);
  for (int i = 0; i < 4; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    down += w.contact ? 1 : 0;
    const double y = w.centre.y - v.params().tyre_radius[size_t(i)] - info.lift;
    in.wheel_ground_y[size_t(i)] = c * y + s * w.centre.z;
  }
  in.contact = down / 4.0;
  if (ground)
    in.ground_below = [ground](double x, double y, double z, double& gy) {
      drive::Hit hit;
      if (!ground->raycast({x, y, z}, {0, -1, 0}, 60, hit)) return false;
      gy = y - hit.distance;
      return true;
    };
  const drive::CameraView view = rig.update(dt, in);
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
    // Prop origins in the scenery's frame, like the positions.
    w.instance.reserve(mesh.instance.size());
    for (const auto& p : mesh.instance) w.instance.push_back({float(p[0] - origin_x), p[1], float(p[2] - origin_z)});
    for (const auto& m : mesh.materials)
      w.materials.push_back({m.albedo, uint8_t(m.blend), m.far, uint8_t(world::decal_effect(m.effect) ? 1 : 0),
                             uint8_t(m.layers), m.layer_b, m.splat, m.tiling, m.fade});
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
               "           --shadows off|low|high --view chase|front --hud --no-reverse-z\n"
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
               "           --menu-png file.png --map-png prefix (the pause map and the minimap at the end)\n"
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
  uint64_t traffic_seed = 1;
  // --contacts-log (ours, debugging): every hard body contact against the
  // collision, and per gather the walls nothing drawn stands near.
  std::filesystem::path contacts_log_path;
  bool texture_census = false;
  // Free-roam collectibles (SPEC-FREEROAM-COLLECTIBLES): on unless
  // --no-collectibles or --bench (a smash regathers); --reveal shows the
  // unfound ones on the map; --distance adds metres to the total driven;
  // --map-png writes the pause map and the minimap at the end.
  bool collectibles_on = true, reveal = false;
  double seed_distance = 0;
  std::filesystem::path map_png, career_png;  // --career-png: the career page's windows, then exit
  int gather_route = 0;  // --route: the placements' route (-1: every one)
  int camera_mode = 0;   // --camera: the drive camera to start in (0-4, SPEC-CAMERAS' cycle order)
  // Reversed depth with an infinite far plane (ours; --no-reverse-z: the
  // 0.25 m to 16 km projection).
  bool reverse_z = true;
  // The player's settings (settings.h) go in before the command line, in
  // the window only; --no-settings skips them, --save-settings keeps this
  // run's options for the next.
  std::vector<std::string> args;
  {
    bool window = true, use_settings = true;
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--headless" || a == "--bench" || a == "--menu-png" || a == "--help" || a == "-h") window = false;
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
  bool save_settings = false;
  std::filesystem::path mods_root;
  bool mods_off = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string a = args[i];
    auto next = [&]() -> const char* { return i + 1 < args.size() ? args[++i].c_str() : ""; };
    if (a == "--drive" || a == "--no-settings") continue;
    if (a == "--save-settings") save_settings = true;
    else if (a == "--mods") mods_root = next(), mods_off = false;
    else if (a == "--no-mods") mods_off = true;
    else if (a == "--headless") headless = true;
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
    else if (a == "--no-radio") radio_on = false;
    else if (a == "--event") race_event = next();
    else if (a == "--difficulty") {
      if (!race::parse_difficulty(next(), race_options.difficulty)) {
        std::fprintf(stderr, "carview: --difficulty easy|med|hard|pro\n");
        return 2;
      }
    } else if (a == "--no-rubberband") race_options.rubberband = false;
    else if (a == "--race-auto") race_auto = 1;
    else if (a == "--race-replay") race_options.replay = next();
    else if (a == "--race-seed") race_options.seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--radio-press") radio_press_every = std::atof(next());
    else if (a == "--traffic") traffic_flag = 1;
    else if (a == "--no-traffic") traffic_flag = 0;
    else if (a == "--hour") hour = std::fmod(std::fmod(std::atof(next()), 24.0) + 24.0, 24.0);
    else if (a == "--traffic-seed") traffic_seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--audio-out") audio_out = next();
    else if (a == "--async-stream") async_stream = true;
    else if (a == "--contacts-log") contacts_log_path = next();
    else if (a == "--texture-census") texture_census = true;
    else if (a == "--route") gather_route = std::atoi(next());
    else if (a == "--camera") camera_mode = std::atoi(next());
    else if (a == "--no-reverse-z") reverse_z = false;
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
    } else if (a == "--help" || a == "-h") {
      drive_usage();
      return 0;
    } else {
      std::fprintf(stderr, "carview: unknown option %s\n", a.c_str());
      drive_usage();
      return 2;
    }
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
      } else {
        st.menu_title = "autoshow  137/137  available 13,225,340 CR";
        for (const char* l : {"R1 999  FER_F40Competizione_89  1,000,000 CR", "B 437  MIT_LancerEvoXGSR_08  52,000 CR"})
          st.menu.push_back(l);
      }
      st.menu_selected = 1;
    }
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
  // Mods (cars/mods.h): the window's from the user folder's mods/, a
  // headless run's only from --mods; --no-mods turns them off.
  if (!mods_off) {
    if (mods_root.empty() && !headless) mods_root = carview::default_mods_root();
    const auto mods = cars::mods::list(mods_root);
    cars::mods::activate(mods, bake_dir, sound_dir);
    for (const auto& m : mods)
      std::printf("mod: %s (%s), %d files, %s\n", m.name.c_str(), m.title.c_str(), m.files,
                  m.enabled ? "on" : "off (mod.txt: enabled 0)");
  }
  // The career garage (SPEC-GARAGE-UPGRADES): garage.txt beside the save,
  // made by the profile import; the cars the bake has, by Data_Car id.
  garage::Garage career_garage;
  bool have_garage = false;
  std::filesystem::path garage_path;
  if (!save_path.empty()) garage_path = save_path.parent_path() / "garage.txt";
  const auto baked_ids = bake_dir.empty() ? std::map<int64_t, std::string>{} : baked_car_ids(bake_dir);
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
  // The parts database (the merged one: SHIFT_STATE_ROOT's packages when
  // set), opened in the background; the class bands from the disc.
  std::shared_future<std::shared_ptr<cars::PartsDb>> parts_db;
  std::vector<garage::ClassBand> class_bands;
  if (have_garage && !world_dir.empty()) {
    const auto game_root = world_dir.parent_path().parent_path().parent_path();
    std::string e;
    if (!garage::load_class_bands(game_root, class_bands, &e)) std::fprintf(stderr, "carview: note: %s\n", e.c_str());
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    const std::string state_root = state ? state : "";
    parts_db = std::async(std::launch::async, [game_root, state_root]() -> std::shared_ptr<cars::PartsDb> {
                 cars::Install install;
                 auto db = std::make_shared<cars::PartsDb>();
                 std::string why;
                 if (!cars::discover_install(game_root, state_root, "", install, &why) || !db->open(install, &why)) {
                   std::fprintf(stderr, "carview: note: no parts database: %s\n", why.c_str());
                   return nullptr;
                 }
                 return db;
               }).share();
  }
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
    auto r = std::make_unique<carview::RaceView>(bake_dir);
    if (!r->open(world_dir.parent_path().parent_path().parent_path(), id, race_options, why)) return false;
    race = std::move(r);
    race->player_spawn(spawn);
    have_spawn = true;
    race_top_speed = 0;
    if (wristbands.levels.empty()) {
      std::string e;
      if (!race::load_wristband_tables(world_dir.parent_path().parent_path().parent_path(), wristbands, &e))
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
  }
  // The career page's records (career_page.h): the pause menu's page and
  // --career-png.
  auto career_summary = [&](std::vector<cars::PackInfo>& packs) {
    carview::CareerSummary cs;
    if (wristbands.levels.empty() && !world_dir.empty()) {
      std::string e;
      race::load_wristband_tables(world_dir.parent_path().parent_path().parent_path(), wristbands, &e);
    }
    const int li = wristbands.level_index(career.wristband_points);
    if (li >= 0) cs.level_name = wristbands.levels[size_t(li)].second;
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
      const HudImage img = draw_hud(st, 2);
      const std::string f = career_png.string() + "_" + std::to_string(n) + ".png";
      if (!write_png(f, img.width, img.height, 4, img.rgba.data())) return 1;
      std::printf("career: %s\n", f.c_str());
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
  // drive with it where the last car stood (window and SDL kept). With a
  // save from the player's profile, only the owned cars the bake has (the
  // garage, barn finds included).
  std::vector<std::string> baked_cars;
  std::vector<int64_t> pick_ids;  // the garage Id of each, with a garage
  std::set<std::string> all_baked;  // every baked car (the Autoshow's)
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
      const auto ids = baked_car_ids(bake_dir);
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
  if (!world_dir.empty()) {
    std::vector<race::EventSummary> all;
    std::string e;
    if (race::list_events(world_dir.parent_path().parent_path().parent_path(), all, &e))
      for (const auto& ev : all)
        if (ev.ribbon >= 0) event_ids.push_back(ev.horizon_id);
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
  // The radio (SPEC-RADIO) also outlives car changes: its stations keep
  // playing. The popup logos by station.
  audio::RadioData radio_data;
  std::unique_ptr<audio::Radio> radio;
  bool radio_tried = false;
  struct Logo {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
  };
  std::map<std::string, Logo> radio_logos;
  for (bool again = true; again;) {
  again = false;
  {
    Renderer renderer;
    Showroom showroom(renderer);
    // SHIFT_NO_CULL=1: every world draw, for comparison with the culled frame.
    if (const char* e = std::getenv("SHIFT_NO_CULL"); e && *e == '1') showroom.set_world_culling(false);
    ShowroomOptions options;
    // The garage car's paint: its stored colour Sequence (SPEC-AUTOSHOW-PAINT).
    if (have_garage)
      if (const garage::Car* gc = career_garage.selected_car())
        if (auto it = baked_ids.find(gc->car_id); it != baked_ids.end() && lower_ascii(it->second) == lower_ascii(car))
          options.match_sequence = gc->colour;
    Renderer::Options render_options{window, validation, msaa};
    render_options.reverse_z = reverse_z;
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
    if (!showroom.load(bake_dir, car, options, error) || !renderer.init(render_options, error) ||
        !showroom.upload(error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      status = 1;
    } else {
      const auto& info = showroom.car();
      std::vector<std::string> physics_notes;
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
                std::printf("garage: car %lld (%s) with its parts, %.0f kg (%.1f s)\n", (long long)gc->id,
                            it->second.c_str(), rows.get("List_UpgradeCarBodyWeight.Mass", 0),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return drive::params_for_fitted_car(info, bake_dir, rows, &physics_notes);
              }
              physics_notes.push_back("garage parts not applied: " + e);
            }
        return drive::params_for_baked_car(info, bake_dir, &physics_notes);
      }());
      for (const auto& n : physics_notes) std::fprintf(stderr, "carview: physics: %s\n", n.c_str());
      // The engine sound: the baked loops, mixed at 48 kHz.
      constexpr int kAudioRate = 48000;
      // Ours: the radio's level against the engine (SPEC-RADIO: about 20 dB
      // over the idling engine; the game measured 0.08 to 0.10 RMS), from
      // headless idle and cruise captures.
      constexpr double kRadioMaster = 1.0;  // radio 0.12 RMS, idle engine 0.022 (1.8 clipped under throttle)
      audio::BakedSound baked_sound;
      std::unique_ptr<audio::EnginePlayer> engine_sound;
      std::unique_ptr<audio::RadioPlayer> radio_player;
      if (sound && (!headless || !audio_out.empty())) {
        std::string sound_error;
        if (audio::load_baked_sound(sound_dir, car, baked_sound, &sound_error) &&
            !baked_sound.loaded.sound.groups.empty()) {
          engine_sound = std::make_unique<audio::EnginePlayer>(baked_sound.loaded.sound, baked_sound.table(),
                                                               kAudioRate, vehicle.params().idle_rpm,
                                                               vehicle.params().redline_rpm);
          engine_sound->master = 0.6;  // ours, by the mix's peaks
          std::printf("engine sound: %zu groups from %s\n", baked_sound.loaded.sound.groups.size(),
                      cars::mods::resolve(sound_dir / car).string().c_str());
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
          gather_options.route = gather_route;
          if (!flat_world && !no_far) gather_options.far_radius = 3000;
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
          if (collectibles_on && !bench && !flat_world) {
            const auto game_root = world_dir.parent_path().parent_path().parent_path();
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
              free_roam = std::make_unique<freeroam::FreeRoam>(collectibles, progress);
              gather_options.hidden = hidden_sent = free_roam->hidden();
              std::printf("free roam: %d of %zu signs, %d of %zu barn finds, %.1f km driven\n",
                          free_roam->signs_found(), collectibles.signs.size(), free_roam->barns_found(),
                          collectibles.barns.size(), progress.total_distance / 1000);
            }
            if (!map_tried) {
              map_tried = true;
              std::string why;
              if (!map_view.open(game_root, &why)) std::fprintf(stderr, "carview: note: no map: %s\n", why.c_str());
            }
          }
          // The radio: read once, played with the sound (as the engine).
          if (radio_on && sound && !bench && (!headless || !audio_out.empty())) {
            const auto game_root = world_dir.parent_path().parent_path().parent_path();
            if (!radio_tried) {
              radio_tried = true;
              std::string why;
              if (audio::load_radio(game_root, radio_data, &why)) {
                radio = std::make_unique<audio::Radio>(radio_data, std::random_device{}());
                radio->select(radio_data.cycle[0]);  // Bass Arena (the spec's interim)
                std::printf("radio: %zu stations in the cycle, %zu tracks, %zu voice clips\n", radio_data.cycle.size(),
                            radio_data.music.size(), radio_data.vo.size());
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
              } else {
                std::fprintf(stderr, "carview: note: no radio: %s\n", why.c_str());
              }
            }
            if (radio) {
              radio_player = std::make_unique<audio::RadioPlayer>(*radio, kAudioRate);
              radio_player->master = kRadioMaster;
            }
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
      }
      if (status == 0 && streamer && (traffic_flag == 1 || (traffic_flag < 0 && !headless && !race))) {
        // The test driver keeps to the right lane among traffic (SPEC-TRAFFIC,
        // Driving a lane: 2.2 m right of the centre line).
        follower.set_keep_right(2.2);
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
      // R and the headless driver's "stuck": a clear spot on the nav road.
      carview::SafeReset safe_reset;
      bool safe_reset_ok = false;
      if (status == 0 && streamer) {
        std::string why;
        safe_reset_ok = safe_reset.open(world_dir.parent_path().parent_path().parent_path(), &why);
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
      bool save_due = false;  // a collectible: the window saves at once
      // One free-roam step: the rules, then their notices and the props
      // they hide.
      auto free_roam_step = [&]() {
        if (!free_roam) return;
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
        for (const auto& ev : free_roam->step(cs)) {
          char line[160];
          if (ev.kind == freeroam::Event::kSignSmashed) {
            const int n = free_roam->signs_found();
            std::snprintf(line, sizeof line, "discount sign %d/%zu - upgrades %d percent off", n, collectibles.signs.size(),
                          freeroam::upgrade_discount(n));
            std::printf("t %.2f s: %s smashed (%s)\n", vehicle.time(), collectibles.signs[size_t(ev.index)].id.c_str(), line);
          } else if (ev.kind == freeroam::Event::kRumourStarted) {
            std::snprintf(line, sizeof line, "barn find rumour - M for the map");
            std::printf("t %.2f s: %s rumour at %.1f km driven\n", vehicle.time(),
                        collectibles.barns[size_t(ev.index)].name.c_str(), progress.total_distance / 1000);
          } else {
            const auto& b = collectibles.barns[size_t(ev.index)];
            std::snprintf(line, sizeof line, "barn find! %s is in the garage", barn_car_label(b).c_str());
            std::printf("t %.2f s: %s found: %s (Data_Car %d) to the garage\n", vehicle.time(), b.name.c_str(),
                        barn_car_label(b).c_str(), b.car_id);
          }
          notice = line;
          notice_until = vehicle.time() + 4;
          save_due = true;
        }
        if (streamer && free_roam->hidden() != hidden_sent) streamer->set_hidden(hidden_sent = free_roam->hidden());
      };
      bool have_touch = false;
      double last_touch_y = 0;  // where the wheels last touched, for the fall check
      auto step = [&](const drive::Input& user_in) {
        const drive::Input in = race ? race->player_input(vehicle, user_in) : user_in;
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
        if (traffic) traffic->step(vehicle);
        if (race && streamer)
          race->after_step(vehicle, *ground, streamer->centre_x(), streamer->centre_z(),
                           streamer->collision_radius(), traffic.get());
        if (race && race->session().started()) {
          race_top_speed = std::max(race_top_speed, std::fabs(vehicle.speed()));
          if (const race::RaceAward* a = race->award(career, wristbands, race_top_speed)) {
            const int lvl = wristbands.level_index(career.wristband_points);
            std::printf("race: award %lld CR, %d wristband points (total %d, %s), credits %lld%s%s\n",
                        (long long)a->credits, a->wristband_points, career.wristband_points,
                        lvl >= 0 ? wristbands.levels[size_t(lvl)].second.c_str() : "no wristband",
                        (long long)career.credits, a->first_completion ? ", first completion" : "",
                        a->prize_car ? (", prize car " + std::to_string(a->prize_car)).c_str() : "");
            save_due = true;  // the window saves the award at once
          }
        }
        free_roam_step();
        if (!streamer) return true;
        // Headless runs put a car that is stuck or rolled back on the
        // nearest road (the windowed mode has R).
        if (headless && !race && follower.stuck(vehicle, in)) {
          const drive::Vec3 at = vehicle.position();
          double away = 0;
          if (safe_reset_ok && safe_reset.place(*streamer, vehicle, &away))
            std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away\n", vehicle.time(), at.x,
                        at.y, at.z, away);
          else if (world::Spawn sp; world::recover(*streamer, vehicle, &sp))
            std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away (nearest road "
                        "triangle)\n", vehicle.time(), at.x, at.y, at.z, sp.distance);
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
              notice = "off the world: put back on the road";
              notice_until = vehicle.time() + 3;
            }
            last_touch_y = vehicle.position().y;
          }
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
        else showroom.set_instances({});
        if (race) {
          std::vector<Showroom::Instance> more;
          race->instances(info, vehicle, more);
          showroom.add_instances(more);
        }
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
        s.owned_garage = have_saved && saved.owned_garage;
        if (have_saved) s.carry_from(saved);
        std::string e;
        if (!drive::write_save(save_path, s, &e)) std::fprintf(stderr, "carview: %s\n", e.c_str());
        else if (headless) std::printf("save (%s): %s at %.1f %.1f %.1f\n", why, save_path.string().c_str(), at.x, at.y, at.z);
        saved = s;
        have_saved = true;
      };
      WheelPose poses[4];
      CameraPull camera_pull;
      // SPEC-CAMERAS' rig (the old chase when it cannot load, and for
      // --view front).
      drive::DriveCamera rig;
      bool have_rig = false;
      double rig_min_distance = 1e9, rig_max_distance = 0;  // the chase eye from the CoM, over the run
      if (!world_dir.empty()) {
        std::string note;
        have_rig = load_camera_rig(world_dir.parent_path().parent_path().parent_path(), bake_dir, info, vehicle, rig, note);
        if (!have_rig) std::fprintf(stderr, "carview: note: the camera spec's rig is off: %s\n", note.c_str());
        else if (!note.empty()) std::fprintf(stderr, "carview: note: camera: %s\n", note.c_str());
        rig.set_mode(drive::CameraMode(std::clamp(camera_mode, 0, drive::kCameraModes - 1)));
      }
      // The frame for the drive's camera: the rig, else the old chase.
      auto drive_frame = [&](float aspect, double dt) {
        if (have_rig && !front) {
          bool interior = false;
          double d = 0;
          ShowroomFrame f = rig_frame(rig, info, vehicle, ground, dt, aspect, renderer.reverse_z(), &interior, &d);
          if (rig.mode() == drive::CameraMode::kFollowLow || rig.mode() == drive::CameraMode::kFollowHigh) {
            rig_min_distance = std::min(rig_min_distance, d);
            rig_max_distance = std::max(rig_max_distance, d);
          }
          // No interior is baked yet: the cockpit view leaves the car out.
          showroom.set_draw_car(!interior);
          return f;
        }
        showroom.set_draw_car(true);
        return chase_frame(info, vehicle, aspect, front, ground, &camera_pull, dt, renderer.reverse_z());
      };
      // The car's name and the keys, over the HUD for the first seconds.
      constexpr double kIntroSeconds = 6;
      // (The display name is a string-table key: the media name is shown.)
      const std::string intro = car + " - [ ] pick a car  R back on the road  T time of day  Esc menu";
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
      carview::Autoshow autoshow;
      carview::CareerPage career_page;
      auto discount = [&]() {
        return freeroam::upgrade_discount(free_roam ? free_roam->signs_found() : int(saved.signs_found.size()));
      };
      auto set_hud = [&](int height, const std::string& banner, const PauseMenu* menu = nullptr) {
        HudState st;
        st.banner = banner;
        if (menu) menu->fill(st);
        if (shop.is_open()) shop.fill(class_bands, discount(), career.credits, st.menu_title, st.menu, st.menu_selected);
        if (autoshow.is_open()) autoshow.fill(class_bands, career.credits, st.menu_title, st.menu, st.menu_selected);
        if (career_page.is_open()) career_page.fill(st.menu_title, st.menu, st.menu_selected);
        st.kmh = vehicle.speed() * 3.6;
        st.gear = vehicle.gear();
        st.rpm = vehicle.rpm();
        st.redline_rpm = vehicle.params().redline_rpm;
        st.abs = vehicle.abs_active();
        const int scale = hud_scale(height);
        HudImage img = draw_hud(st, scale);
        const int margin = -8 * scale;
        hud_height = int(img.height);
        renderer.set_overlay(std::move(img.rgba), img.width, img.height, margin, margin);
      };
      // The radio's popup (overlay layer 3), right-aligned above the HUD
      // panel; redrawn when what it shows changes.
      std::string popup_key;
      auto set_radio_popup = [&](int height) {
        const audio::Radio::Popup p = radio ? radio->popup() : audio::Radio::Popup{};
        if (p.alpha <= 0 || p.station < 0) {
          if (!popup_key.empty()) renderer.set_overlay({}, 0, 0, 0, 0, 3);
          popup_key.clear();
          return;
        }
        const auto& st = radio_data.stations[size_t(p.station)];
        RadioPopupState ps;
        ps.off = st.off;
        if (p.track >= 0) ps.title = st.tracks[size_t(p.track)].title, ps.artist = st.tracks[size_t(p.track)].artist;
        if (auto it = radio_logos.find(st.name); it != radio_logos.end())
          ps.logo = it->second.rgba.data(), ps.logo_width = it->second.width, ps.logo_height = it->second.height;
        ps.alpha = std::round(p.alpha * 16) / 16;
        const std::string key = st.name + "|" + std::to_string(p.track) + "|" + std::to_string(ps.alpha) + "|" +
                                std::to_string(height) + "|" + std::to_string(hud_height);
        if (key == popup_key) return;
        popup_key = key;
        const int scale = hud_scale(height);
        HudImage img = draw_radio_popup(ps, height);
        renderer.set_overlay(std::move(img.rgba), img.width, img.height, -8 * scale,
                             -(8 * scale + hud_height + 4 * scale), 3);
      };
      auto radio_press = [&](int dir) {
        if (!radio) return;
        radio->press(dir);
        const auto& st = radio_data.stations[size_t(radio->selected())];
        std::printf("t %.2f s: radio %s\n", vehicle.time(), st.off ? "off" : st.name.c_str());
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
        sc.data = progress_loaded ? &collectibles : nullptr;
        sc.progress = progress_loaded ? &progress : nullptr;
        sc.reveal = reveal;
        if (race) race->opponent_positions(sc.opponents);
        return sc;
      };
      double minimap_at = -1;
      auto set_minimap = [&](int height, bool force) {
        if (!map_view.ready() || !streamer) return;
        const double t = vehicle.time();
        if (!force && minimap_at >= 0 && t >= minimap_at && t - minimap_at < 1.0 / 15) return;
        minimap_at = t;
        const int scale = hud_scale(height);
        HudImage img = map_view.minimap(map_scene(), std::max(96, height * 26 / 100));
        renderer.set_overlay(std::move(img.rgba), img.width, img.height, 8 * scale, -8 * scale, 1);
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
          showroom.set_braking(brake_glow(vehicle));
          place_world();
          showroom.set_frame(drive_frame(float(width) / float(height), 2 * drive::Vehicle::kStep));
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
        double race_done_at = -1;
        for (int s = 0; s <= steps && status == 0; ++s) {
          const auto t0 = std::chrono::steady_clock::now();
          const bool ok =
              s == 0 || step(cruise ? follower.input(vehicle) : drive::script_input(script, vehicle.time()));
          if (radio && radio_press_every > 0 && s > 0) {
            const double t = vehicle.time(), was = t - drive::Vehicle::kStep;
            if (std::floor(t / radio_press_every) > std::floor(was / radio_press_every)) radio_press(+1);
          }
          if ((engine_sound || radio_player) && s > 0) {
            // One physics step of sound, the inputs set once per step; the
            // radio adds to the engine.
            const int frames = int(std::lround(kAudioRate * drive::Vehicle::kStep));
            const size_t at = audio_track.size();
            audio_track.resize(at + 2 * size_t(frames), 0.0f);
            if (engine_sound) {
              engine_sound->set(sound_inputs(front));
              engine_sound->render(audio_track.data() + at, frames);
            }
            if (radio_player) radio_player->render(audio_track.data() + at, frames);
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
          showroom.set_wheel_poses(poses);
          showroom.set_braking(brake_glow(vehicle));
          place_world();
          showroom.set_frame(drive_frame(float(width) / float(height), every));
          if (hud) {
            set_hud(int(height), vehicle.time() < notice_until  ? notice
                                 : race ? race_banner()
                                 : vehicle.time() < kIntroSeconds ? intro
                                                                  : std::string());
            set_minimap(int(height), true);
            set_radio_popup(int(height));
          }
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
        if (race) {
          std::printf("%s m\nrace: results at %.2f s%s\n%s", race->summary().c_str(), race->session().clock(),
                      race->session().all_finished() ? "" : " (not every car finished)", race->results().c_str());
        }
        if (radio) {
          const auto& st = radio_data.stations[size_t(radio->selected())];
          std::printf("radio: %s at the end, %.1f s on the radio clock\n", st.off ? "off" : st.name.c_str(),
                      radio->clock());
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
        uint64_t last = SDL_GetTicksNS(), title_at = 0;
        double accumulator = 0;
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
        for (size_t k = 0; k < baked_cars.size(); ++k)
          if (pick_ids.empty() ? baked_cars[k] == lower_ascii(car) : pick_ids[k] == career_garage.selected) pick = k;
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
          if (!pick_ids.empty()) {
            career_garage.selected = pick_ids[pick];
            write_career_garage();
          }
          running = false;
        };
        bool show_hud = true;
        PauseMenu menu;
        double next_save = 30;  // s of drive time: the save is also written every 30 s
        // The engine sound's stream: about 60 ms queued ahead.
        SDL_AudioStream* audio_stream = nullptr;
        if (engine_sound || radio_player) {
          const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
          audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
          if (audio_stream) SDL_ResumeAudioStreamDevice(audio_stream);
          else std::fprintf(stderr, "carview: no audio device: %s\n", SDL_GetError());
        }
        std::vector<float> audio_block;
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
        };
        // The pause map: M, or the pause menu's item; the drive waits while
        // it is open.
        bool map_open = false, map_dirty = false;
        double map_zoom = 2, map_cx = 0, map_cz = 0;
        int map_w = 0, map_h = 0;
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
          popup_key.clear();
          save_now("map");
        };
        auto close_map = [&]() {
          map_open = false;
          renderer.set_overlay({}, 0, 0, 0, 0, 2);
          minimap_at = -1;
        };
        // The chosen item: resume, the map, the picker, back on the road, or quit.
        auto choose = [&]() {
          const int item = menu.selected;
          menu.open = false;
          if (item == PauseMenu::kMap) open_map();
          if (item == PauseMenu::kChangeCar) browse(0);
          if (item == PauseMenu::kUpgrades || item == PauseMenu::kAutoshow) {
            std::string why = "the garage needs a career (--import-profile)";
            bool ready = false;
            if (have_garage && parts_db.valid()) {
              why = "loading the parts database, try again";
              if (parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                why = "no parts database";
                if (const auto db = parts_db.get())
                  ready = item == PauseMenu::kAutoshow
                              ? autoshow.open(*db, all_baked, why)
                              : shop.open(*db, career_garage,
                                          career_garage.selected_car() ? career_garage.selected_car()->id : 0, why);
              }
            }
            if (!ready) {
              notice = why;
              notice_until = vehicle.time() + 3;
            }
          }
          if (item == PauseMenu::kEvent && !event_ids.empty()) {
            next_event = event_ids[event_pick];
            running = false;
          }
          if (item == PauseMenu::kCareer) {
            std::vector<cars::PackInfo> packs;
            const carview::CareerSummary cs = career_summary(packs);
            career_page.open(cs, std::move(packs));
          }
          if (item == PauseMenu::kReset) put_back();
          if (item == PauseMenu::kQuit) running = false;
        };
        auto set_event_label = [&]() {
          menu.event_label = event_ids.empty() ? std::string("race (none)")
                                               : "< race " + event_ids[event_pick] + " >";
        };
        auto pick_event = [&](int by) {
          if (event_ids.empty()) return;
          const int n = int(event_ids.size());
          event_pick = size_t(((int(event_pick) + by) % n + n) % n);
          set_event_label();
        };
        // After the player's finish (SPEC-EVENT-REWARDS, Results screen):
        // Continue puts the car at the route's post-race spot (or where it
        // stands) in free roam; Restart runs the event again.
        auto race_continue = [&]() {
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
          next_event = race_event;
          running = false;
        };
        auto open_menu = [&]() {
          set_event_label();
          menu.show();
          picking = false;
          save_now("pause");
        };
        while (running) {
          SDL_Event e;
          while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            // The map takes the keys and buttons while open.
            if (map_open) {
              bool close = false;
              const double pan = 1500 / map_zoom;
              auto zoom_by = [&](double f) {
                map_zoom = std::clamp(map_zoom * f, 1.0, 24.0);
                map_dirty = true;
              };
              auto pan_by = [&](double dx, double dz) {
                map_cx += dx, map_cz += dz;
                map_dirty = true;
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (!e.key.repeat && (k == SDLK_M || k == SDLK_ESCAPE)) close = true;
                if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) zoom_by(1.5);
                if (k == SDLK_MINUS || k == SDLK_KP_MINUS) zoom_by(1 / 1.5);
                if (k == SDLK_LEFT || k == SDLK_A) pan_by(-pan, 0);
                if (k == SDLK_RIGHT || k == SDLK_D) pan_by(pan, 0);
                if (k == SDLK_UP || k == SDLK_W) pan_by(0, pan);
                if (k == SDLK_DOWN || k == SDLK_S) pan_by(0, -pan);
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_BACK)
                  close = true;
                if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) zoom_by(1.5);
                if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) zoom_by(1 / 1.5);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) pan_by(-pan, 0);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) pan_by(pan, 0);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) pan_by(0, pan);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) pan_by(0, -pan);
              }
              if (close) close_map();
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
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) fit();
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) leave_shop();
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) shop.move_slot(-1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) shop.move_slot(1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) shop.move_part(-1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) shop.move_part(1);
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) fit();
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
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) autoshow.close();
                if (k == SDLK_UP || k == SDLK_W) autoshow.move(-1);
                if (k == SDLK_DOWN || k == SDLK_S) autoshow.move(1);
                if (k == SDLK_LEFT || k == SDLK_A) autoshow.jump_class(-1);
                if (k == SDLK_RIGHT || k == SDLK_D) autoshow.jump_class(1);
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) buy();
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) autoshow.close();
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) autoshow.move(-1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) autoshow.move(1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) autoshow.jump_class(-1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) autoshow.jump_class(1);
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) buy();
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
                if (k == SDLK_ESCAPE) menu.open = false;
                if (k == SDLK_UP || k == SDLK_W) menu.move(-1);
                if (k == SDLK_DOWN || k == SDLK_S) menu.move(1);
                if (menu.selected == PauseMenu::kEvent && (k == SDLK_LEFT || k == SDLK_A)) pick_event(-1);
                if (menu.selected == PauseMenu::kEvent && (k == SDLK_RIGHT || k == SDLK_D)) pick_event(1);
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) choose();
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) menu.open = false;
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) menu.move(-1);
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu.move(1);
                if (menu.selected == PauseMenu::kEvent && b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) pick_event(-1);
                if (menu.selected == PauseMenu::kEvent && b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) pick_event(1);
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
              if (e.key.key == SDLK_M) open_map();
              if (e.key.key == SDLK_COMMA) radio_press(-1);
              if (e.key.key == SDLK_PERIOD) radio_press(+1);
              // V: SwitchCamera (the rig); without the rig, the front view.
              if (e.key.key == SDLK_V) {
                if (have_rig) rig.cycle();
                else front = !front;
              }
              if (e.key.key == SDLK_LEFTBRACKET) browse(-1);
              if (e.key.key == SDLK_RIGHTBRACKET) browse(1);
              const bool results = race && race->player_finished() && !picking;
              if (results && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) race_continue();
              else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) take_pick();
              if (results && e.key.key == SDLK_Y) race_restart();
              if (e.key.key == SDLK_T && traffic) {
                const bool back = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                traffic->set_hour(std::fmod(traffic->hour() + (back ? 23 : 1), 24.0));
                char line_buf[64];
                std::snprintf(line_buf, sizeof line_buf, "traffic at %02d:00", int(traffic->hour()));
                notice = line_buf;
                notice_until = vehicle.time() + 3;
              }
              // F12: the pose for a report (carview-pose-N.txt in the working
              // folder): where, which way, when and in what.
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
                  std::snprintf(line_buf, sizeof line_buf, "pose saved: %s", name);
                } else {
                  std::snprintf(line_buf, sizeof line_buf, "cannot write %s", name);
                }
                notice = line_buf;
                notice_until = vehicle.time() + 3;
              }
              if (e.key.key == SDLK_H) {
                show_hud = !show_hud;
                if (!show_hud)
                  for (int layer : {0, 1, 3}) renderer.set_overlay({}, 0, 0, 0, 0, layer);
                popup_key.clear();
                minimap_at = -1;
              }
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) open_menu();
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) open_map();
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) radio_press(-1);
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) radio_press(+1);
              // The right shoulder is SwitchCamera (SPEC-CAMERAS: the default
              // layout); in the picker it pages forward. The left one opens
              // the picker and pages back.
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) browse(-1);
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                if (picking || !have_rig) browse(1);
                else rig.cycle();
              }
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) take_pick();
              if (race && race->player_finished() && !picking) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) race_continue();
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) race_restart();
              }
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
          double rig_look = camera_pull.look_target;
          for (SDL_Gamepad* pad : pads) {
            auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
            const double steer = deadzone(axis(SDL_GAMEPAD_AXIS_LEFTX), 0.10, 0.95);
            const double throttle = deadzone(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.03, 0.97);
            const double brake = deadzone(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.03, 0.97);
            const double look = deadzone(axis(SDL_GAMEPAD_AXIS_RIGHTX), 0.20, 0.95);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH)) camera_pull.look_target = 3.14159265358979;
            else if (look != 0) camera_pull.look_target = -look * 3.14159265358979;
            // The rig's look (SPEC-CAMERAS, Button): the right stick down
            // looks back, left and right to the sides, its click straight back.
            const double lx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ly = axis(SDL_GAMEPAD_AXIS_RIGHTY);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK) || (ly > 0.5 && ly > std::fabs(lx)))
              rig_look = 3.14159265358979;
            else if (std::fabs(lx) > 0.5 && std::fabs(lx) >= std::fabs(ly))
              rig_look = (lx > 0 ? 0.5 : -0.5) * 3.14159265358979;
            if (steer != 0 || throttle > 0 || brake > 0 || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) {
              in.steer = steer;
              in.throttle = throttle;
              in.brake = brake;
              in.handbrake = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
              break;
            }
          }
          rig.set_look(rig_look);
          const uint64_t now = SDL_GetTicksNS();
          const double frame_dt = std::min(0.25, double(now - last) * 1e-9);
          // Paused: the car holds still (no steps, no time to catch up).
          if (!menu.open && !map_open && !shop.is_open() && !autoshow.is_open() && !career_page.is_open()) accumulator += frame_dt;
          last = now;
          if (vehicle.time() >= next_save) {
            save_now("timer");
            next_save = vehicle.time() + 30;
          }
          if (save_due && !map_open) {  // a collectible: the game's micro-save
            save_due = false;
            save_now("collectible");
          }
          while (!menu.open && !map_open && !shop.is_open() && !autoshow.is_open() && !career_page.is_open() &&
                 accumulator >= drive::Vehicle::kStep) {
            drawn_before = vehicle.pose();
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
            {
              const drive::Vec3 at = vehicle.position();
              const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
              std::snprintf(title, sizeof title,
                            "carview drive - %s - %3.0f km/h  gear %s  %5.0f rpm%s   at %.0f %.0f %.0f heading %.0f",
                            car.c_str(), std::fabs(vehicle.speed()) * 3.6, gear, vehicle.rpm(),
                            vehicle.abs_active() ? "  ABS" : "", at.x, at.y, at.z, std::atan2(f.x, f.z) * 57.29578);
            }
            if (picking) {
              const size_t n = std::strlen(title);
              std::snprintf(title + n, sizeof title - n, "   |   pick %zu/%zu: %s (Enter or X to drive it)", pick + 1,
                            baked_cars.size(), baked_cars[pick].c_str());
            }
            SDL_SetWindowTitle(window, title);
          }
          if (audio_stream) {
            if (engine_sound) engine_sound->set(sound_inputs(front));
            const int queued = SDL_GetAudioStreamQueued(audio_stream) / int(2 * sizeof(float));
            const int frames = int(0.06 * kAudioRate) - queued;
            if (frames > 0) {
              audio_block.assign(2 * size_t(frames), 0.0f);
              if (engine_sound) engine_sound->render(audio_block.data(), frames);
              if (radio_player) radio_player->render(audio_block.data(), frames);
              SDL_PutAudioStreamData(audio_stream, audio_block.data(), int(audio_block.size() * sizeof(float)));
            }
          }
          int w = 0, h = 0;
          SDL_GetWindowSizeInPixels(window, &w, &h);
          if (map_open) {
            if (w != map_w || h != map_h) map_dirty = true;
            if (map_dirty && w > 64 && h > 64) {
              map_dirty = false;
              map_w = w, map_h = h;
              const int m = std::max(16, h / 30);
              HudImage img = map_view.pause_map(map_scene(), w - 2 * m, h - 2 * m, map_zoom, map_cx, map_cz);
              renderer.set_overlay(std::move(img.rgba), img.width, img.height, m, m, 2);
            }
          } else if (show_hud) {
            set_minimap(h, false);
            char line[160] = "";
            if (picking)
              std::snprintf(line, sizeof line, "car %zu/%zu: %s - Enter drives it", pick + 1, baked_cars.size(),
                            baked_cars[pick].c_str());
            set_hud(h,
                    picking                          ? std::string(line)
                    : vehicle.time() < notice_until  ? notice
                    : race ? race_banner()
                                 : vehicle.time() < kIntroSeconds ? intro
                                                     : std::string(),
                    &menu);
            set_radio_popup(h);  // after the panel: its height
          }
          // The car and the camera at the frame's time: between the last
          // two steps (Vehicle::Pose), put back after the frame is built.
          const drive::Vehicle::Pose stepped = vehicle.pose();
          vehicle.set_pose(drive::Vehicle::blend(drawn_before, stepped, accumulator / drive::Vehicle::kStep));
          if (race) race->set_blend(accumulator / drive::Vehicle::kStep);
          wheel_poses(vehicle, poses);
          showroom.set_wheel_poses(poses);
          showroom.set_braking(brake_glow(vehicle));
          place_world();
          showroom.set_frame(drive_frame(h > 0 ? float(w) / float(h) : 1.0f, frame_dt));
          vehicle.set_pose(stepped);
          if (!renderer.draw_frame(FrameConstants{}, error)) {
            std::fprintf(stderr, "carview: %s\n", error.c_str());
            status = 1;
            break;
          }
        }
        for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
        if (audio_stream) SDL_DestroyAudioStream(audio_stream);
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
      if (free_roam)
        std::printf("free roam: %d of %zu signs (upgrades %d%% off), %d of %zu barn finds, %.1f km driven\n",
                    free_roam->signs_found(), collectibles.signs.size(),
                    freeroam::upgrade_discount(free_roam->signs_found()), free_roam->barns_found(),
                    collectibles.barns.size(), progress.total_distance / 1000);
      save_now("end");
      if (traffic) std::printf("%s\n", traffic->summary().c_str());
      if (validation) std::printf("validation messages: %u\n", renderer.validation_messages());
      if (status == 0 && renderer.validation_messages()) status = 3;
    }
  }
  if (status == 0 && leave_race) {
    leave_race = false;
    if (race) {
      race.reset();
      race_event.clear();
      std::printf("race: left for free roam\n");
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
    }
    again = true;
    SDL_SetWindowTitle(window, ("carview drive - loading " + id).c_str());
  }
  if (status == 0 && !next_car.empty()) {
    if (race) {
      std::printf("race: left for a car change\n");
      race.reset();
    }
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
