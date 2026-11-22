// Livery.zip vinyl & decal parser (1,372 kind-3 & kind-1 .carbin vector shapes),
// 1024x1024 LiveryMasks layer-stack compositor, and .liv persistence.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cars/bake_format.h"
#include "cars/shader_settings.h"
#include "fh1data/archive.h"

namespace cars {

struct VinylVertex {
  float x = 0.0f;
  float y = 0.0f;
  uint32_t argb = 0xFFFFFFFFu;
};

struct VinylMesh {
  std::string name;
  uint32_t kind = 0;  // 1 or 3
  float min_x = -0.25f, min_y = -0.25f, max_x = 0.25f, max_y = 0.25f;
  std::vector<VinylVertex> vertices;
  std::vector<uint32_t> indices;  // triangle list (3 per tri)
  bool is_decal = false;          // true if vertex colours carry multi-colour artwork
};

// Parses a raw vinyl or decal .carbin payload from Livery.zip (both kind 3 and kind 1).
bool parse_vinyl_carbin(const std::vector<uint8_t>& data, VinylMesh& out, std::string* error = nullptr);

// Opens media/Livery.zip, enumerates all 1,372 vinyl & decal shapes, and loads
// & caches VinylMesh geometry on demand.
class LiveryLibrary {
 public:
  bool open(const std::filesystem::path& livery_zip, std::string* error = nullptr);
  bool is_open() const { return archive_ != nullptr; }
  const std::vector<std::string>& vinyl_names() const { return vinyls_; }
  const std::vector<std::string>& decal_names() const { return decals_; }
  size_t total_shapes() const { return vinyls_.size() + decals_.size(); }
  const VinylMesh* load_shape(const std::string& member_path, std::string* error = nullptr);

 private:
  std::unique_ptr<fh1::Archive> archive_;
  std::vector<std::string> vinyls_;
  std::vector<std::string> decals_;
  std::map<std::string, VinylMesh> cache_;
};

// Projection plane on the car's LiveryMasks atlas.
enum class LiveryPlaneKind : int {
  kFullAtlas = 0,  // direct 0..1 livery UV space
  kTop = 1,        // hood / roof / trunk plane
  kLeft = 2,       // left side
  kRight = 3,      // right side
  kFront = 4,      // front bumper
  kRear = 5,       // rear bumper
  kCount = 6,
};

const char* livery_plane_name(LiveryPlaneKind p);

struct VinylLayer {
  std::string shape_path;  // e.g. "vinyls/a_01.carbin", "decals/decal001.carbin", or "@stripe"/"@circle"/"@star"
  LiveryPlaneKind plane = LiveryPlaneKind::kFullAtlas;
  float x = 0.5f;         // normalized center X in [0..1]
  float y = 0.5f;         // normalized center Y in [0..1]
  float scale_x = 0.25f;  // normalized width
  float scale_y = 0.25f;  // normalized height
  float rot_deg = 0.0f;   // rotation in degrees
  float skew = 0.0f;
  uint32_t rgb = 0xFFFFFFu;
  float alpha = 1.0f;
  bool flip_x = false;
  bool flip_y = false;
  bool visible = true;
};

struct LiveryDocument {
  std::string name = "Custom Livery";
  int64_t car_id = 0;
  CarPaintSpec paint;
  std::vector<VinylLayer> layers;
};

// Composites all visible layers of `doc` onto a `size x size` (default 1024x1024)
// RGBA8 texture in LiveryMasks UV space. When `lib` is null or a shape is a
// built-in primitive ("@rect", "@circle", "@stripe", "@star", "@chevron"),
// falls back cleanly to crisp built-in vector geometry.
bool composite_livery(const LiveryDocument& doc, LiveryLibrary* lib, const std::vector<bake::LiveryPlane>* planes,
                      std::vector<uint8_t>& out_rgba, uint32_t size = 1024);

// Saves/loads a LiveryDocument to/from a .liv file (human-readable shift-livery 1
// format, plus graceful import of raw FH1 binary .liv headers).
bool save_livery_file(const std::filesystem::path& path, const LiveryDocument& doc, std::string* error = nullptr);
bool load_livery_file(const std::filesystem::path& path, LiveryDocument& out, std::string* error = nullptr);

}  // namespace cars
