// carview reference mode: the per-car reference JSON and the M1 metric.
//
// Schema `shift-engine.m1-reference.v1` (AUTOSHOW_REFERENCE, "Reference
// files"): one JSON per car at .local/re/cars/refs/<car_id>.json, with the
// captured image and the car-coverage mask beside it. The fields read here:
//
//   media_name      the car's folder (Data_Car.MediaName)
//   car_id          Data_Car.Id
//   width, height   image size in pixels
//   clip_from_car   16 numbers, column-major, column vectors: car space to
//                   upright Direct3D clip (+y up, z/w 0 at near), with the
//                   Direct3D 9 half pixel removed, so a standard viewport
//                   gives the game's pixels
//   image, mask     PNGs relative to the JSON (the frame without UI, and
//                   the pixels a car-only replay changes by 6/255 or more)
//   image_car_only  the car-only replay (optional)
//   paint_rgb       the colour shown (sRGB 0-255; the first Combo_Colors row)
//   checks.transparent_clip_from_car
//                   the glass and lamp covers' matrix (optional)
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace carview {

struct Reference {
  std::string schema, media_name;
  int64_t car_id = 0;
  uint32_t width = 0, height = 0;
  float clip_from_car[16] = {};
  // Glass and lamp covers: checks.transparent_clip_from_car, or a copy of
  // clip_from_car when the reference has none.
  float transparent_clip_from_car[16] = {};
  bool has_transparent = false;
  // Resolved against the JSON's folder; empty if absent. image_car_only is
  // the replay with only the car's draws (the masked-PSNR target).
  std::filesystem::path image, mask, image_car_only;
  bool has_paint = false;
  std::array<int, 3> paint_rgb{};
};

bool read_reference(const std::filesystem::path& path, Reference& out, std::string& error);

// The car-space eye of a perspective clip matrix (the point that maps to
// x = y = w = 0). False for a matrix without one.
bool clip_eye(const float m[16], float eye[3]);

// The M1 metric between a reference and our render (RGBA8 images, masks 0
// or nonzero), all the same size.
struct Metric {
  double iou = 0;           // silhouette IoU of the two masks
  double masked_psnr = 0;   // PSNR (RGB) over the reference mask's pixels; inf if equal
  double edge_overlap = 0;  // share of reference silhouette edge pixels with an
                            // engine edge pixel within 1 px (provisional)
  uint64_t reference_pixels = 0, engine_pixels = 0;
};
Metric compare(const std::vector<uint8_t>& reference_rgba, const std::vector<uint8_t>& reference_mask,
               const std::vector<uint8_t>& engine_rgba, const std::vector<uint8_t>& engine_mask,
               uint32_t width, uint32_t height);

}  // namespace carview
