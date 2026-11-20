// carview --bake: the showroom over carbake's output, windowed or headless,
// and the reference mode.
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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include "reference.h"
#include "renderer.h"
#include "showroom.h"

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
        // by 6/255 or more in any channel against an empty one. Drawn the
        // same way at the silhouette: clear to the capture's empty
        // background (its top-left pixel); a partly covered pixel counts
        // when our colour moves 6/255 or more. A fully covered pixel always
        // counts, so our shading being near the background grey (a
        // material error, not geometry) does not cut holes in the mask.
        // --coverage-mask counts any coverage instead.
        bool diff_mask = false;
        uint8_t background[3] = {0, 0, 0};
        if (reference_mode && !coverage_mask && !against.empty()) {
          std::vector<uint8_t> target;
          std::string ignored;
          if (read_png(against, 4, width, height, target, ignored)) {
            for (int c = 0; c < 3; ++c) background[c] = target[size_t(c)];
            showroom.set_background(background[0] / 255.0f, background[1] / 255.0f, background[2] / 255.0f);
            diff_mask = true;
          }
        }
        std::vector<uint8_t> rgba, mask;
        if (!renderer.render_offscreen(FrameConstants{}, width, height, rgba, error) ||
            ((!mask_out.empty() || !against.empty()) ? !showroom.render_mask(width, height, mask, error, diff_mask)
                                                      : false)) {
          std::fprintf(stderr, "carview: %s\n", error.c_str());
          status = 1;
        } else {
          // The background is opaque; the alpha channel holds blending
          // leftovers, so write it as 255.
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (diff_mask) {
            for (size_t p = 0; p < mask.size(); ++p) {
              if (mask[p] == 0 || mask[p] == 255) continue;
              bool changed = false;
              for (int c = 0; c < 3; ++c)
                changed |= std::abs(int(rgba[p * 4 + size_t(c)]) - int(background[c])) >= 6;
              mask[p] = changed ? 255 : 0;
            }
          }
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

}  // namespace carview
