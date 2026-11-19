// carview --world: one sector of Colorado drawn from the render models.
//
//   carview --world <members-dir|bin.zip> --frame <frame.txt>
//           [--box x0 z0 x1 z1] [--out-dir dir] [--capture file.ppm]...
//           [--skip text]... [--nearest-lod] [--recentre]
//           [--feature text]... [--edge-percentile P] [--near metres]
//           [--list] [--probe x y]... [--validation]
//
// --skip drops mesh objects whose name holds the text (any case).
// --nearest-lod keeps, of the objects whose names differ only in a "LODnn"
// number, those with the lowest number (which LOD the game draws is an open
// question of the spec; this is a choice for comparisons, not the game's
// rule). --recentre rebuilds the matrix's translation from the camera line
// (a derived variant, for matrices printed with few decimals). --feature
// scores the edges of the mesh objects whose name holds the text. --list
// prints the drawn models and objects; --probe prints the file-frame
// position and the mesh object drawn at a pixel.
//
// The frame file holds what was measured from a game frame (keep it out of
// the repository):
//
//   matrix m00 m01 ... m33   the scene matrix, row-major: clip = M * (x, y, z, 1)
//                            with (x, y, z) in the file frame
//   size W H                 image size (default 1280 720)
//   viewport sx ox sy oy     presented pixel = (ox + sx * x/w, oy + sy * y/w);
//                            default (-W/2, W/2, H/2, H/2): the half-turn
//                            mapping of the spec's "Screen" paragraph
//   camera x y z             camera in the file frame (optional; lighting,
//                            and --recentre)
//   ignore x0 y0 x1 y1       a pixel rectangle the comparison skips (the
//                            player's car, the HUD); repeatable
//
// Writes world-colour.png (flat shading by face normal, a tint per model),
// world-depth.png (log view distance, near is bright, sky black),
// world-depth.pfm (z/w as float), and per capture (binary PPM) an overlay
// of our edges on the capture's edges and one of the visible triangle
// edges, with alignment numbers: for our silhouette and object-boundary
// pixels, the distance to the nearest edge of the capture, unshifted, at a
// 20 px shift (a baseline) and at the shift that fits best.
// Spec: WORLD-GEOMETRY (horizon1-recomp docs/formats/WORLD_GEOMETRY.md).

#include "world_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "car_model.h"
#include "fh1data/archive.h"
#include "fh1data/rmb.h"
#include "renderer.h"

namespace fs = std::filesystem;

namespace carview {
namespace {

using Mat4 = std::array<double, 16>;  // row-major
using Vec4 = std::array<double, 4>;

Vec4 mul(const Mat4& m, const Vec4& v) {
  Vec4 out{};
  for (int r = 0; r < 4; ++r)
    out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3];
  return out;
}

bool invert(const Mat4& m, Mat4& out) {
  // Gauss-Jordan with partial pivoting.
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-15) return false;
    std::swap(a[c], a[pivot]);
    const double inv = 1.0 / a[c][c];
    for (double& v : a[c]) v *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  return true;
}

struct Frame {
  Mat4 matrix{};
  bool has_matrix = false;
  uint32_t width = 1280, height = 720;
  std::array<double, 4> viewport{};  // sx ox sy oy
  bool has_viewport = false;
  std::array<double, 3> camera{};
  bool has_camera = false;
  std::vector<std::array<int, 4>> ignore;
};

bool read_frame(const fs::path& path, Frame& f, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open " + path.string();
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream s(line);
    std::string key;
    if (!(s >> key)) continue;
    auto numbers = [&](double* out, int n) {
      for (int i = 0; i < n; ++i)
        if (!(s >> out[i])) return false;
      return true;
    };
    bool ok = true;
    if (key == "matrix") ok = f.has_matrix = numbers(f.matrix.data(), 16);
    else if (key == "viewport") ok = f.has_viewport = numbers(f.viewport.data(), 4);
    else if (key == "camera") ok = f.has_camera = numbers(f.camera.data(), 3);
    else if (key == "ignore") {
      std::array<int, 4> r{};
      ok = bool(s >> r[0] >> r[1] >> r[2] >> r[3]);
      if (ok) f.ignore.push_back(r);
    } else if (key == "size") ok = bool(s >> f.width >> f.height) && f.width && f.height;
    else ok = false;
    if (!ok) {
      error = "bad frame file line: " + line;
      return false;
    }
  }
  if (!f.has_matrix) {
    error = "the frame file has no matrix";
    return false;
  }
  if (!f.has_viewport) f.viewport = {-0.5 * f.width, 0.5 * f.width, 0.5 * f.height, 0.5 * f.height};
  return true;
}

// Where clip x, y and w are zero: the camera, for a projection centred on it.
bool matrix_camera(const Mat4& m, std::array<double, 3>& out) {
  Mat4 a{};
  const int rows[3] = {0, 1, 3};
  for (int i = 0; i < 3; ++i)
    for (int c = 0; c < 4; ++c) a[i * 4 + c] = m[rows[i] * 4 + c];
  a[15] = 1;
  Mat4 inv;
  if (!invert(a, inv)) return false;
  const Vec4 p = mul(inv, {0, 0, 0, 1});
  if (std::fabs(p[3]) < 1e-12) return false;
  out = {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
  return true;
}

// --- sources -----------------------------------------------------------------

struct Source {
  std::unique_ptr<fh1::Archive> archive;
  std::vector<std::string> names;               // display names
  std::vector<fs::path> files;                  // directory source
  std::vector<const fh1::ArchiveMember*> members;  // archive source

  bool open(const fs::path& path, std::string& error) {
    auto is_model = [](std::string n) {
      for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
      return n.size() > 8 && n.compare(n.size() - 8, 8, ".rmb.bin") == 0;
    };
    if (fs::is_directory(path)) {
      for (const auto& e : fs::recursive_directory_iterator(path))
        if (e.is_regular_file() && is_model(e.path().filename().string())) files.push_back(e.path());
      std::sort(files.begin(), files.end());
      for (const auto& f : files) names.push_back(f.filename().string());
      return true;
    }
    archive = fh1::Archive::open(path, &error);
    if (!archive) return false;
    std::set<std::string> seen;
    for (const auto& m : archive->members()) {
      const std::string n = fh1::normalize_member_name(m.name);
      if (is_model(n) && seen.insert(n).second) {
        members.push_back(&m);
        names.push_back(fs::path(m.name).filename().string());
      }
    }
    return true;
  }
  size_t size() const { return names.size(); }
  bool read(size_t i, std::vector<uint8_t>& out, std::string& error) {
    if (archive) {
      auto r = archive->read(*members[i]);
      if (!r.ok()) {
        error = r.error;
        return false;
      }
      out = std::move(r.data);
      return true;
    }
    std::ifstream f(files[i], std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(f), {});
    return bool(f) || f.eof();
  }
};

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// --- images ------------------------------------------------------------------

struct Rgb {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> pixels;  // RGB8
};

bool read_ppm(const fs::path& path, Rgb& out, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  std::string magic;
  int max_value = 0;
  auto token = [&](std::string& t) {
    t.clear();
    char c;
    while (f.get(c)) {
      if (c == '#') {
        while (f.get(c) && c != '\n') {}
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!t.empty()) return true;
        continue;
      }
      t += c;
    }
    return !t.empty();
  };
  std::string w, h, m;
  if (!token(magic) || magic != "P6" || !token(w) || !token(h) || !token(m)) {
    error = path.string() + ": not a binary PPM";
    return false;
  }
  out.width = uint32_t(std::stoul(w));
  out.height = uint32_t(std::stoul(h));
  max_value = std::stoi(m);
  if (max_value != 255) {
    error = path.string() + ": only 8-bit PPM is read";
    return false;
  }
  out.pixels.resize(size_t(out.width) * out.height * 3);
  f.read(reinterpret_cast<char*>(out.pixels.data()), std::streamsize(out.pixels.size()));
  if (!f) {
    error = path.string() + ": truncated";
    return false;
  }
  return true;
}

bool write_png(const fs::path& path, uint32_t w, uint32_t h, int channels,
               const std::vector<uint8_t>& pixels) {
  return stbi_write_png(path.string().c_str(), int(w), int(h), channels, pixels.data(),
                        int(w) * channels) != 0;
}

// Squared Euclidean distance transform (Felzenszwalb and Huttenlocher).
void edt_1d(const float* f, float* d, int n, std::vector<int>& v, std::vector<float>& z) {
  int k = 0;
  v[0] = 0;
  z[0] = -1e20f;
  z[1] = 1e20f;
  for (int q = 1; q < n; ++q) {
    float s;
    while (true) {
      s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * float(q - v[k]));
      if (s <= z[k] && k > 0) --k;
      else break;
    }
    ++k;
    v[k] = q;
    z[k] = s;
    z[k + 1] = 1e20f;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < float(q)) ++k;
    d[q] = float(q - v[k]) * float(q - v[k]) + f[v[k]];
  }
}

std::vector<float> distance_to(const std::vector<uint8_t>& mask, int w, int h) {
  std::vector<float> grid(size_t(w) * h);
  for (size_t i = 0; i < grid.size(); ++i) grid[i] = mask[i] ? 0.0f : 1e10f;
  const int n = std::max(w, h);
  std::vector<float> f(n), d(n), z(n + 1);
  std::vector<int> v(n);
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) f[y] = grid[size_t(y) * w + x];
    edt_1d(f.data(), d.data(), h, v, z);
    for (int y = 0; y < h; ++y) grid[size_t(y) * w + x] = d[y];
  }
  for (int y = 0; y < h; ++y) {
    edt_1d(&grid[size_t(y) * w], d.data(), w, v, z);
    for (int x = 0; x < w; ++x) grid[size_t(y) * w + x] = std::sqrt(d[x]);
  }
  return grid;
}

// Thin edges of an image: Sobel magnitude on luminance, non-maximum
// suppression across the gradient, then a threshold at a percentile.
std::vector<uint8_t> image_edges(const Rgb& img, double percentile, double* threshold_out) {
  const int w = int(img.width), h = int(img.height);
  std::vector<float> lum(size_t(w) * h), mag(size_t(w) * h, 0.0f), gx(mag), gy(mag);
  for (size_t i = 0; i < lum.size(); ++i)
    lum[i] = 0.299f * img.pixels[i * 3] + 0.587f * img.pixels[i * 3 + 1] +
             0.114f * img.pixels[i * 3 + 2];
  auto L = [&](int x, int y) { return lum[size_t(y) * w + x]; };
  for (int y = 1; y < h - 1; ++y)
    for (int x = 1; x < w - 1; ++x) {
      const float sx = (L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1)) -
                       (L(x - 1, y - 1) + 2 * L(x - 1, y) + L(x - 1, y + 1));
      const float sy = (L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1)) -
                       (L(x - 1, y - 1) + 2 * L(x, y - 1) + L(x + 1, y - 1));
      const size_t i = size_t(y) * w + x;
      gx[i] = sx;
      gy[i] = sy;
      mag[i] = std::sqrt(sx * sx + sy * sy);
    }
  std::vector<float> sorted(mag);
  std::nth_element(sorted.begin(), sorted.begin() + size_t(percentile * (sorted.size() - 1)),
                   sorted.end());
  const float threshold = std::max(1.0f, sorted[size_t(percentile * (sorted.size() - 1))]);
  if (threshold_out) *threshold_out = threshold;
  std::vector<uint8_t> edges(size_t(w) * h, 0);
  for (int y = 1; y < h - 1; ++y)
    for (int x = 1; x < w - 1; ++x) {
      const size_t i = size_t(y) * w + x;
      if (mag[i] < threshold) continue;
      const float ax = std::fabs(gx[i]), ay = std::fabs(gy[i]);
      int dx = 0, dy = 0;
      if (ax > 2.414f * ay) dx = 1;
      else if (ay > 2.414f * ax) dy = 1;
      else {
        dx = 1;
        dy = (gx[i] > 0) == (gy[i] > 0) ? 1 : -1;
      }
      const float a = mag[size_t(y - dy) * w + (x - dx)], b = mag[size_t(y + dy) * w + (x + dx)];
      // Ties (a step between two pixels) keep one side only.
      if (mag[i] > a && mag[i] >= b) edges[i] = 1;
    }
  return edges;
}

// --- the scene ---------------------------------------------------------------

struct Scene {
  std::vector<Vertex> vertices;  // a triangle list, de-indexed with face normals
  std::vector<uint32_t> vertex_object;  // per vertex: the mesh object
  std::vector<std::string> object_names;
  size_t models = 0, objects = 0, model_vertices = 0, triangles = 0, skipped_objects = 0;
};

// One 64 x 32 texture: a tint per model on the left, and on the right an
// identity colour per mesh object. The car pipeline lights a texel as
// base * k + s with the same k and s on every channel, so (R - G, G - B)
// over a constant blue still names the object once k is fixed: the
// identity pass gives every vertex the light's direction as its normal
// (k = 1.1 for every pixel the camera sees).
constexpr uint32_t kTextureWidth = 64, kTextureHeight = 32;
constexpr uint32_t kIdSide = 22;  // 22 x 22 identities
constexpr float kIdBase = 0.08f, kIdStep = 0.025f;

std::array<uint8_t, 3> tint(uint32_t k) {
  uint32_t h = k * 2654435761u;
  h ^= h >> 15;
  const float hue = float(h % 360) / 60.0f;
  const float c = 0.35f, x = c * (1 - std::fabs(std::fmod(hue, 2.0f) - 1)), base = 0.55f;
  float r = 0, g = 0, b = 0;
  switch (int(hue)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  return {uint8_t((base + r) * 255), uint8_t((base + g) * 255), uint8_t((base + b) * 255)};
}

fh1::RgbaImage make_texture() {
  fh1::RgbaImage t;
  t.width = kTextureWidth;
  t.height = kTextureHeight;
  t.rgba.assign(size_t(t.width) * t.height * 4, 255);
  for (uint32_t y = 0; y < kTextureHeight; ++y)
    for (uint32_t x = 0; x < kTextureWidth; ++x) {
      uint8_t* p = &t.rgba[(size_t(y) * kTextureWidth + x) * 4];
      if (x < 32) {
        const auto c = tint(y * 32 + x);
        std::memcpy(p, c.data(), 3);
      } else {
        const uint32_t i = x - 32, j = y;
        p[0] = uint8_t(std::lround((kIdBase + kIdStep * float(i)) * 255));
        p[1] = uint8_t(std::lround((kIdBase + kIdStep * float(j)) * 255));
        p[2] = uint8_t(std::lround(kIdBase * 255));
      }
    }
  return t;
}

std::array<float, 2> tint_uv(uint32_t k) {
  k %= 32 * 32;
  return {(float(k % 32) + 0.5f) / kTextureWidth, (float(k / 32) + 0.5f) / kTextureHeight};
}

std::array<float, 2> id_uv(uint32_t id) {
  id %= kIdSide * kIdSide;
  return {(float(32 + id % kIdSide) + 0.5f) / kTextureWidth,
          (float(id / kIdSide) + 0.5f) / kTextureHeight};
}

// The identity under a pixel of the identity pass, or -1.
int32_t decode_id(const uint8_t* p) {
  const double scale = 1.1 * 255.0 * kIdStep;
  const double j = (double(p[1]) - double(p[2])) / scale;
  const double i_minus_j = (double(p[0]) - double(p[1])) / scale;
  const long jj = std::lround(j), ii = std::lround(i_minus_j + double(jj));
  if (std::fabs(j - double(jj)) > 0.3 || std::fabs(i_minus_j + double(jj) - double(ii)) > 0.3)
    return -1;
  if (ii < 0 || jj < 0 || ii >= long(kIdSide) || jj >= long(kIdSide)) return -1;
  return int32_t(jj * kIdSide + ii);
}

struct EdgePixel {
  int x, y;
  double w;
  uint32_t object;
  bool silhouette;  // a depth break or sky; else a boundary between mesh objects
};

struct Score {
  size_t n = 0;
  double mean = 0, median = 0, within1 = 0, within2 = 0, within3 = 0;
};

}  // namespace

int run_world(int argc, char** argv) {
  fs::path source_path, frame_path, out_dir = ".";
  std::vector<fs::path> captures;
  std::vector<std::string> skips, features;
  double box[4] = {1500, -1460, 2500, -460};  // x0 z0 x1 z1
  std::vector<std::array<int, 2>> probes;
  double edge_percentile = 0.95, near_limit = 40.0;
  bool list = false, nearest_lod = false;
  bool recentre = false, validation = std::getenv("SHIFT_VALIDATION") != nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--world" && (v = next())) source_path = v;
    else if (a == "--frame" && (v = next())) frame_path = v;
    else if (a == "--out-dir" && (v = next())) out_dir = v;
    else if (a == "--capture" && (v = next())) captures.push_back(v);
    else if (a == "--skip" && (v = next())) skips.push_back(lower(v));
    else if (a == "--feature" && (v = next())) features.push_back(lower(v));
    else if (a == "--edge-percentile" && (v = next())) edge_percentile = std::atof(v) / 100.0;
    else if (a == "--near" && (v = next())) near_limit = std::atof(v);
    else if (a == "--recentre") recentre = true;
    else if (a == "--list") list = true;
    else if (a == "--nearest-lod") nearest_lod = true;
    else if (a == "--validation") validation = true;
    else if (a == "--probe" && i + 2 < argc) {
      probes.push_back({std::atoi(argv[i + 1]), std::atoi(argv[i + 2])});
      i += 2;
    } else if (a == "--box" && i + 4 < argc) {
      for (double& b : box) b = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr,
                   "usage: carview --world <members-dir|bin.zip> --frame <frame.txt>\n"
                   "               [--box x0 z0 x1 z1] [--out-dir dir] [--capture file.ppm]...\n"
                   "               [--skip text]... [--nearest-lod] [--recentre]\n"
                   "               [--feature text]... [--edge-percentile P] [--near metres]\n"
                   "               [--list] [--probe x y]... [--validation]\n");
      return 2;
    }
  }
  if (source_path.empty() || frame_path.empty()) {
    std::fprintf(stderr, "carview --world: a source and --frame are required\n");
    return 2;
  }
  std::string error;
  Frame frame;
  if (!read_frame(frame_path, frame, error)) {
    std::fprintf(stderr, "carview: %s\n", error.c_str());
    return 1;
  }
  const uint32_t W = frame.width, H = frame.height;
  const double sx = frame.viewport[0], ox = frame.viewport[1], sy = frame.viewport[2],
               oy = frame.viewport[3];
  fs::create_directories(out_dir);

  // The camera: where the matrix puts clip x = y = w = 0.
  std::array<double, 3> solved{};
  if (matrix_camera(frame.matrix, solved)) {
    std::printf("camera from the matrix: %.3f %.3f %.3f", solved[0], solved[1], solved[2]);
    if (frame.has_camera)
      std::printf(" (frame file: %.3f %.3f %.3f)", frame.camera[0], frame.camera[1],
                  frame.camera[2]);
    std::printf("\n");
  }
  Mat4 M = frame.matrix;
  if (recentre) {
    // Derived variant: keep the 3x3 part, rebuild the translation from the
    // camera, assuming a projection centred on it (clip x = y = w = 0 at
    // the camera, clip z there = the z row's offset minus the w row's).
    if (!frame.has_camera) {
      std::fprintf(stderr, "carview: --recentre needs a camera line\n");
      return 2;
    }
    const double z_at_camera = frame.matrix[11] - frame.matrix[15];
    for (int r = 0; r < 4; ++r) {
      double t = 0;
      for (int c = 0; c < 3; ++c) t -= M[r * 4 + c] * frame.camera[c];
      M[r * 4 + 3] = t + (r == 2 ? z_at_camera : 0.0);
    }
    std::printf("recentred translation (derived): %.6f %.6f %.6f %.6f\n", M[3], M[7], M[11],
                M[15]);
  }
  const std::array<double, 3> eye = frame.has_camera ? frame.camera : solved;

  // Load every world-space model whose header box overlaps the rectangle.
  Source source;
  if (!source.open(source_path, error)) {
    std::fprintf(stderr, "carview: %s\n", error.c_str());
    return 1;
  }
  size_t failed = 0, local_overlapping = 0;
  std::vector<std::pair<std::string, fh1::RenderModel>> selected;
  for (size_t i = 0; i < source.size(); ++i) {
    std::vector<uint8_t> bytes;
    fh1::RenderModel model;
    if (!source.read(i, bytes, error) || !fh1::parse_render_model(bytes, model, &error)) {
      ++failed;
      std::fprintf(stderr, "carview: %s: %s\n", source.names[i].c_str(), error.c_str());
      continue;
    }
    const bool overlaps = model.box_max[0] >= box[0] && model.box_min[0] <= box[2] &&
                          model.box_max[2] >= box[1] && model.box_min[2] <= box[3];
    if (!overlaps) continue;
    if (!fh1::rmb_is_world_space(model)) {
      ++local_overlapping;
      continue;
    }
    selected.emplace_back(source.names[i], std::move(model));
  }

  // Object filters: --skip, then --nearest-lod over what is left.
  auto skipped = [&](const std::string& name) {
    const std::string n = lower(name);
    return std::any_of(skips.begin(), skips.end(),
                       [&](const std::string& s) { return n.find(s) != std::string::npos; });
  };
  auto lod_family = [](const std::string& name, int& lod) {
    std::string n = lower(name);
    for (size_t at = n.find("lod"); at != std::string::npos; at = n.find("lod", at + 1)) {
      if (at + 4 < n.size() && std::isdigit(static_cast<unsigned char>(n[at + 3])) &&
          std::isdigit(static_cast<unsigned char>(n[at + 4]))) {
        lod = (n[at + 3] - '0') * 10 + (n[at + 4] - '0');
        n[at + 3] = n[at + 4] = '*';
        return n;
      }
    }
    lod = -1;
    return n;
  };
  std::map<std::string, int> lowest_lod;
  for (const auto& [name, model] : selected)
    for (const auto& o : model.objects) {
      if (skipped(o.name)) continue;
      int lod;
      const std::string family = lod_family(o.name, lod);
      if (lod < 0) continue;
      auto [it, added] = lowest_lod.emplace(family, lod);
      if (!added) it->second = std::min(it->second, lod);
    }

  Scene scene;
  for (const auto& [model_name, model] : selected) {
    const uint32_t model_index = uint32_t(scene.models++);
    if (list)
      std::printf("model %s: box %.1f %.1f %.1f .. %.1f %.1f %.1f\n", model_name.c_str(),
                  model.box_min[0], model.box_min[1], model.box_min[2], model.box_max[0],
                  model.box_max[1], model.box_max[2]);
    const auto uv = tint_uv(model_index);
    for (const auto& o : model.objects) {
      int lod;
      const std::string family = lod_family(o.name, lod);
      if (skipped(o.name) || (nearest_lod && lod >= 0 && lowest_lod[family] != lod)) {
        ++scene.skipped_objects;
        continue;
      }
      ++scene.objects;
      if (list)
        std::printf("  object %s: %u vertices, stride %u\n", o.name.c_str(), o.vertex_count,
                    o.stride);
      const uint32_t object_id = uint32_t(scene.object_names.size());
      scene.object_names.push_back(model_name + " " + o.name);
      scene.model_vertices += o.vertex_count;
      for (const auto& s : o.submeshes) {
        const auto tri = fh1::rmb_triangle_list(s);
        for (size_t t = 0; t + 2 < tri.size(); t += 3) {
          const auto a = o.position(tri[t]), b = o.position(tri[t + 1]), c = o.position(tri[t + 2]);
          const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
          const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
          float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0]};
          const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          if (!(len > 0)) continue;
          for (float& k : n) k /= len;
          for (const auto* p : {&a, &b, &c}) {
            Vertex vx{};
            std::memcpy(vx.position, p->data(), sizeof vx.position);
            std::memcpy(vx.normal, n, sizeof vx.normal);
            vx.uv[0] = uv[0];
            vx.uv[1] = uv[1];
            vx.occlusion = 1.0f;
            vx.kind = float(MaterialKind::kTextured);
            scene.vertices.push_back(vx);
            scene.vertex_object.push_back(object_id);
          }
          ++scene.triangles;
        }
      }
    }
  }
  std::printf("sector x %.0f..%.0f z %.0f..%.0f: %zu world-space models, %zu mesh objects drawn "
              "(%zu skipped), %zu vertices, %zu triangles; %zu local-space models overlap and "
              "are not drawn; %zu members failed\n",
              box[0], box[2], box[1], box[3], scene.models, scene.objects, scene.skipped_objects,
              scene.model_vertices, scene.triangles, local_overlapping, failed);
  if (scene.vertices.empty()) {
    std::fprintf(stderr, "carview: nothing to draw\n");
    return 1;
  }
  if (scene.objects > kIdSide * kIdSide)
    std::printf("note: %zu mesh objects share %u identity colours; boundaries between objects "
                "with the same colour are not seen\n",
                scene.objects, kIdSide * kIdSide);

  // The game's clip space to Vulkan's. Vulkan maps x/w to W/2 + W/2 * x/w
  // and y/w to H/2 + H/2 * y/w (y down); fold the frame's viewport mapping
  // into the x and y rows. z/w and w are the game's.
  Mat4 vk = M;
  for (int c = 0; c < 4; ++c) {
    vk[0 * 4 + c] = (sx * M[0 * 4 + c] + (ox - 0.5 * W) * M[3 * 4 + c]) / (0.5 * W);
    vk[1 * 4 + c] = (sy * M[1 * 4 + c] + (oy - 0.5 * H) * M[3 * 4 + c]) / (0.5 * H);
  }
  FrameConstants constants{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) constants.view_projection[c * 4 + r] = float(vk[r * 4 + c]);
  const double light[3] = {0.35, 0.85, 0.4};
  const double light_len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
  for (int k = 0; k < 3; ++k) {
    constants.light_direction[k] = float(light[k] / light_len);
    constants.eye[k] = float(eye[k]);
  }
  CarModel mesh;
  mesh.vertices = scene.vertices;
  mesh.indices.resize(scene.vertices.size());
  for (size_t i = 0; i < mesh.indices.size(); ++i) mesh.indices[i] = uint32_t(i);
  mesh.texture_levels.push_back(make_texture());

  Renderer renderer;
  std::vector<uint8_t> rgba, id_rgba;
  std::vector<float> depth, id_depth;
  if (!renderer.init({nullptr, validation, 1}, error) || !renderer.upload(mesh, error) ||
      !renderer.render_offscreen(constants, W, H, rgba, error, &depth)) {
    std::fprintf(stderr, "carview: %s\n", error.c_str());
    return 1;
  }
  // The identity pass: every normal is the light, which points back along
  // the view axis, so every visible pixel is lit the same way.
  {
    double back[3] = {-M[12], -M[13], -M[14]};
    const double len = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
    FrameConstants id_constants = constants;
    for (int k = 0; k < 3; ++k) id_constants.light_direction[k] = float(back[k] / len);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
      Vertex& v = mesh.vertices[i];
      for (int k = 0; k < 3; ++k) v.normal[k] = id_constants.light_direction[k];
      const auto uv = id_uv(scene.vertex_object[i]);
      v.uv[0] = uv[0];
      v.uv[1] = uv[1];
    }
    if (!renderer.replace_geometry(mesh, error) ||
        !renderer.render_offscreen(id_constants, W, H, id_rgba, error, &id_depth)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      return 1;
    }
  }
  std::printf("rendered %ux%u on %s, 1 sample, D32 depth (colour and identity passes)\n", W, H,
              renderer.device_name().c_str());

  // Per pixel: view depth w and the file-frame position, from z/w and the
  // inverse of the game matrix; and the mesh object drawn there.
  Mat4 inverse;
  if (!invert(M, inverse)) {
    std::fprintf(stderr, "carview: the matrix is singular\n");
    return 1;
  }
  const size_t pixels = size_t(W) * H;
  std::vector<double> view_w(pixels, 0.0);
  std::vector<std::array<double, 3>> world(pixels);
  std::vector<int32_t> object_at(pixels, -1);
  double zmin = 1, zmax = 0, wmin = 1e30, wmax = 0;
  size_t covered = 0, undecoded = 0, depth_mismatch = 0;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const size_t i = size_t(y) * W + x;
      const double d = depth[i];
      if (d >= 1.0) continue;
      depth_mismatch += id_depth[i] != depth[i];
      object_at[i] = decode_id(&id_rgba[i * 4]);
      undecoded += object_at[i] < 0;
      const double nx = (x + 0.5 - ox) / sx, ny = (y + 0.5 - oy) / sy;
      const Vec4 h = mul(inverse, {nx, ny, d, 1.0});
      if (!(h[3] > 0)) continue;
      view_w[i] = 1.0 / h[3];
      world[i] = {h[0] / h[3], h[1] / h[3], h[2] / h[3]};
      ++covered;
      zmin = std::min(zmin, d);
      zmax = std::max(zmax, d);
      wmin = std::min(wmin, view_w[i]);
      wmax = std::max(wmax, view_w[i]);
    }
  std::printf("depth drawn: %zu of %zu pixels covered; z/w %.7f..%.7f; view depth w %.3f..%.1f "
              "m; identity pass: %zu pixels undecoded, %zu depth differences\n",
              covered, pixels, zmin, zmax, wmin, wmax, undecoded, depth_mismatch);
  auto object_name = [&](int32_t id) -> std::string {
    if (id < 0) return "?";
    std::string out;
    for (size_t o = size_t(id); o < scene.object_names.size(); o += kIdSide * kIdSide)
      out += (out.empty() ? "" : " | ") + scene.object_names[o];
    return out;
  };
  for (const auto& [px, py] : probes) {
    if (px < 0 || py < 0 || px >= int(W) || py >= int(H)) continue;
    const size_t i = size_t(py) * W + px;
    if (view_w[i] <= 0) {
      std::printf("probe %d %d: sky\n", px, py);
      continue;
    }
    std::printf("probe %d %d: w %.2f m, file frame %.2f %.2f %.2f, %s\n", px, py, view_w[i],
                world[i][0], world[i][1], world[i][2], object_name(object_at[i]).c_str());
  }

  std::vector<uint8_t> colour(pixels * 3), depth_png(pixels, 0);
  for (size_t i = 0; i < pixels; ++i) std::memcpy(&colour[i * 3], &rgba[i * 4], 3);
  const double lmin = std::log(std::max(wmin, 1e-3)), lmax = std::log(std::max(wmax, wmin * 1.001));
  for (size_t i = 0; i < pixels; ++i)
    if (view_w[i] > 0)
      depth_png[i] = uint8_t(std::clamp(
          255.0 - 235.0 * (std::log(view_w[i]) - lmin) / (lmax - lmin), 1.0, 255.0));
  write_png(out_dir / "world-colour.png", W, H, 3, colour);
  write_png(out_dir / "world-depth.png", W, H, 1, depth_png);
  {
    std::ofstream pfm(out_dir / "world-depth.pfm", std::ios::binary);
    pfm << "Pf\n" << W << " " << H << "\n-1.0\n";
    for (uint32_t y = H; y-- > 0;)
      pfm.write(reinterpret_cast<const char*>(&depth[size_t(y) * W]), std::streamsize(W * 4));
  }

  // Our edges. Silhouettes: the near side of a break in 1/w (1/w is affine
  // in screen space over a plane, so a slope alone is not a break) and
  // geometry next to sky. Boundaries: the nearer of two neighbouring pixels
  // of different mesh objects (road against terrain, painted lines).
  std::vector<uint8_t> edge_kind(pixels, 0);  // 1 silhouette, 2 boundary
  auto inv_w = [&](int x, int y) {
    const double w = view_w[size_t(y) * W + x];
    return w > 0 ? 1.0 / w : 0.0;
  };
  for (int y = 0; y < int(H); ++y)
    for (int x = 0; x < int(W); ++x)
      for (int axis = 0; axis < 2; ++axis) {
        const int dx = axis == 0, dy = axis == 1;
        const int x1 = x + dx, y1 = y + dy;
        if (x1 >= int(W) || y1 >= int(H)) continue;
        const size_t i0 = size_t(y) * W + x, i1 = size_t(y1) * W + x1;
        const double a = inv_w(x, y), b = inv_w(x1, y1);
        if ((a > 0) != (b > 0)) {
          edge_kind[a > 0 ? i0 : i1] = 1;
          continue;
        }
        if (a == 0) continue;
        const size_t nearer = a > b ? i0 : i1;
        const int xm = x - dx, ym = y - dy, x2 = x1 + dx, y2 = y1 + dy;
        if (xm >= 0 && ym >= 0 && x2 < int(W) && y2 < int(H)) {
          const double am = inv_w(xm, ym), b2 = inv_w(x2, y2);
          const double tolerance = 0.1 * std::max(a, b);
          if (am > 0 && b2 > 0 && std::fabs(b - (2 * a - am)) > tolerance &&
              std::fabs(a - (2 * b - b2)) > tolerance) {
            edge_kind[nearer] = 1;
            continue;
          }
        }
        if (object_at[i0] >= 0 && object_at[i1] >= 0 && object_at[i0] != object_at[i1] &&
            !edge_kind[nearer])
          edge_kind[nearer] = 2;
      }
  std::vector<uint8_t> ignored(pixels, 0);
  for (const auto& r : frame.ignore)
    for (int y = std::max(0, r[1]); y < std::min(int(H), r[3]); ++y)
      for (int x = std::max(0, r[0]); x < std::min(int(W), r[2]); ++x) ignored[size_t(y) * W + x] = 1;
  std::vector<EdgePixel> our_edges;
  for (int y = 3; y < int(H) - 3; ++y)
    for (int x = 3; x < int(W) - 3; ++x) {
      const size_t i = size_t(y) * W + x;
      if (!edge_kind[i] || ignored[i]) continue;
      our_edges.push_back({x, y, view_w[i], uint32_t(std::max(object_at[i], 0)), edge_kind[i] == 1});
    }

  // Matrix rounding: perturb every entry by up to half a unit in the 4th
  // decimal and see how far visible surface points move.
  {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> noise(-0.5e-4, 0.5e-4);
    std::vector<double> near_moves, mid_moves, far_moves, all_moves;
    for (int trial = 0; trial < 16; ++trial) {
      Mat4 P = M;
      for (double& e : P) e += noise(rng);
      for (uint32_t y = 4; y < H; y += 8)
        for (uint32_t x = 4; x < W; x += 8) {
          const size_t i = size_t(y) * W + x;
          if (view_w[i] <= 0) continue;
          const Vec4 p{world[i][0], world[i][1], world[i][2], 1.0};
          const Vec4 c0 = mul(M, p), c1 = mul(P, p);
          const double dxp = sx * (c1[0] / c1[3] - c0[0] / c0[3]);
          const double dyp = sy * (c1[1] / c1[3] - c0[1] / c0[3]);
          const double move = std::sqrt(dxp * dxp + dyp * dyp);
          all_moves.push_back(move);
          (view_w[i] < 20 ? near_moves : view_w[i] < 100 ? mid_moves : far_moves).push_back(move);
        }
    }
    auto quantile = [](std::vector<double> v, double q) {
      if (v.empty()) return 0.0;
      std::sort(v.begin(), v.end());
      return v[size_t(q * (v.size() - 1))];
    };
    std::printf("matrix rounding (16 draws of +/-0.5e-4 on every entry): pixel shift of visible "
                "points median %.2f, 95th %.2f; by view depth: <20 m %.2f (95th %.2f), 20-100 m "
                "%.2f, >100 m %.2f\n",
                quantile(all_moves, 0.5), quantile(all_moves, 0.95), quantile(near_moves, 0.5),
                quantile(near_moves, 0.95), quantile(mid_moves, 0.5), quantile(far_moves, 0.5));
  }

  // Compare with each capture.
  int status = 0;
  Rgb previous;
  for (const auto& capture_path : captures) {
    Rgb capture;
    if (!read_ppm(capture_path, capture, error)) {
      std::fprintf(stderr, "carview: %s\n", error.c_str());
      status = 1;
      continue;
    }
    if (capture.width != W || capture.height != H) {
      std::fprintf(stderr, "carview: %s is %ux%u, the frame %ux%u\n",
                   capture_path.string().c_str(), capture.width, capture.height, W, H);
      status = 1;
      continue;
    }
    const std::string stem = capture_path.stem().string();
    std::printf("capture %s:\n", capture_path.string().c_str());
    if (!previous.pixels.empty()) {
      size_t differing = 0;
      double total = 0;
      for (size_t i = 0; i < capture.pixels.size(); i += 3) {
        int d = 0;
        for (int k = 0; k < 3; ++k) d += std::abs(int(capture.pixels[i + k]) - previous.pixels[i + k]);
        differing += d > 0;
        total += d;
      }
      std::printf("  against the previous capture: %zu pixels differ, mean |diff| %.3f per "
                  "pixel (RGB sum)\n",
                  differing, total / double(pixels));
    }
    previous = capture;

    double threshold = 0;
    const auto edges = image_edges(capture, edge_percentile, &threshold);
    const auto dist = distance_to(edges, int(W), int(H));
    size_t edge_count = 0;
    for (uint8_t e : edges) edge_count += e;
    std::printf("  capture edges: %zu pixels (Sobel >= %.1f, the %.0fth percentile, thinned)\n",
                edge_count, threshold, edge_percentile * 100);

    auto score = [&](const std::vector<const EdgePixel*>& set, int dx, int dy, bool keep) {
      Score s;
      std::vector<float> d;
      double sum = 0;
      for (const EdgePixel* e : set) {
        const int u = e->x + dx, v = e->y + dy;
        if (u < 0 || v < 0 || u >= int(W) || v >= int(H)) continue;
        const float k = dist[size_t(v) * W + u];
        sum += std::min(k, 10.0f);
        if (keep) d.push_back(k);
        ++s.n;
      }
      s.mean = s.n ? sum / double(s.n) : 1e9;
      if (keep && !d.empty()) {
        std::sort(d.begin(), d.end());
        auto fraction = [&](float r) {
          return double(std::upper_bound(d.begin(), d.end(), r) - d.begin()) / double(d.size());
        };
        s.median = d[d.size() / 2];
        s.within1 = fraction(1.0f);
        s.within2 = fraction(2.0f);
        s.within3 = fraction(3.0f);
      }
      return s;
    };
    auto report = [&](const char* label, const std::vector<const EdgePixel*>& set) {
      if (set.size() < 20) {
        std::printf("  %-28s %6zu pixels (too few to score)\n", label, set.size());
        return;
      }
      const Score s0 = score(set, 0, 0, true);
      int bx = 0, by = 0;
      double best = s0.mean;
      for (int dy = -10; dy <= 10; ++dy)
        for (int dx = -10; dx <= 10; ++dx) {
          const double m = score(set, dx, dy, false).mean;
          if (m < best - 1e-12) {
            best = m;
            bx = dx;
            by = dy;
          }
        }
      auto refine = [](double minus, double centre, double plus) {
        const double denominator = minus - 2 * centre + plus;
        return denominator > 1e-12 ? 0.5 * (minus - plus) / denominator : 0.0;
      };
      const double fx = bx + refine(score(set, bx - 1, by, false).mean, best,
                                    score(set, bx + 1, by, false).mean);
      const double fy = by + refine(score(set, bx, by - 1, false).mean, best,
                                    score(set, bx, by + 1, false).mean);
      const double base_x = score(set, 20, 0, false).mean, base_y = score(set, 0, 20, false).mean;
      std::printf("  %-28s %6zu px  mean %.2f  median %.2f  <=1 %5.1f%%  <=2 %5.1f%%  <=3 %5.1f%%"
                  "  | shifted 20 px: x %.2f y %.2f | best shift (%+d,%+d) mean %.2f, "
                  "sub-pixel (%+.2f,%+.2f)\n",
                  label, s0.n, s0.mean, s0.median, 100 * s0.within1, 100 * s0.within2,
                  100 * s0.within3, base_x, base_y, bx, by, best, fx, fy);
    };
    std::vector<const EdgePixel*> all, sil_near, sil_far, bnd_near, bnd_far;
    for (const auto& e : our_edges) {
      all.push_back(&e);
      const bool near = e.w < near_limit;
      (e.silhouette ? (near ? sil_near : sil_far) : (near ? bnd_near : bnd_far)).push_back(&e);
    }
    std::printf("  our edge pixels against the capture's edges (distance in px to the nearest, "
                "clamped at 10 for the mean; near is w < %.0f m):\n",
                near_limit);
    report("all", all);
    report("silhouettes, near", sil_near);
    report("silhouettes, far", sil_far);
    report("object boundaries, near", bnd_near);
    report("object boundaries, far", bnd_far);
    for (const auto& f : features) {
      std::vector<const EdgePixel*> set;
      for (const auto& e : our_edges) {
        const std::string name = lower(object_name(int32_t(e.object)));
        if (name.find(f) != std::string::npos) set.push_back(&e);
      }
      report(("feature \"" + f + "\"").c_str(), set);
    }

    // The skyline: per column, the first geometry row under sky against the
    // capture's strongest vertical colour step within 15 rows.
    std::vector<int> offsets;
    for (int x = 0; x < int(W); ++x) {
      int top = -1;
      for (int y = 0; y < int(H); ++y)
        if (view_w[size_t(y) * W + x] > 0) {
          top = y;
          break;
        }
      if (top < 16 || top > int(H) - 16 || ignored[size_t(top) * W + x]) continue;
      int best_row = top;
      double best_step = -1;
      for (int y = top - 15; y <= top + 15; ++y) {
        const uint8_t* a = &capture.pixels[(size_t(y - 1) * W + x) * 3];
        const uint8_t* b = &capture.pixels[(size_t(y) * W + x) * 3];
        const double step = std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
        if (step > best_step) {
          best_step = step;
          best_row = y;
        }
      }
      offsets.push_back(best_row - top);
    }
    if (!offsets.empty()) {
      std::vector<int> sorted = offsets;
      std::sort(sorted.begin(), sorted.end());
      size_t within1 = 0, within2 = 0;
      double mean_abs = 0;
      for (int o : offsets) {
        within1 += std::abs(o) <= 1;
        within2 += std::abs(o) <= 2;
        mean_abs += std::abs(o);
      }
      std::printf("  skyline: %zu columns with sky above our geometry; capture's strongest step "
                  "minus our top row: median %+d px, mean |.| %.2f, within 1 px %.1f %%, 2 px "
                  "%.1f %%\n",
                  offsets.size(), sorted[sorted.size() / 2], mean_abs / offsets.size(),
                  100.0 * within1 / offsets.size(), 100.0 * within2 / offsets.size());
    }

    // Overlays: our silhouettes (red) and object boundaries (magenta) over
    // the capture's edges (cyan); and the visible triangle edges (yellow).
    std::vector<uint8_t> overlay(capture.pixels);
    for (size_t i = 0; i < pixels; ++i) {
      uint8_t* p = &overlay[i * 3];
      for (int k = 0; k < 3; ++k) p[k] = uint8_t(p[k] * 0.5);
      if (edges[i]) p[0] = 0, p[1] = 170, p[2] = 170;
      if (edge_kind[i] == 1) p[0] = 255, p[1] = 40, p[2] = 40;
      if (edge_kind[i] == 2) p[0] = 255, p[1] = 60, p[2] = 255;
      if (ignored[i]) p[2] = uint8_t(std::min(255, p[2] + 60));
    }
    write_png(out_dir / ("overlay-edges-" + stem + ".png"), W, H, 3, overlay);

    std::vector<uint8_t> wire(capture.pixels);
    size_t drawn_edges = 0;
    auto plot = [&](int x, int y, double z) {
      if (x < 0 || y < 0 || x >= int(W) || y >= int(H)) return;
      const size_t i = size_t(y) * W + x;
      const double d = depth[i];
      // Visible: no farther than the drawn surface by more than about 3 % of w.
      if (d < 1.0 && z > d + 0.03 * (1.0 - d)) return;
      uint8_t* p = &wire[i * 3];
      p[0] = uint8_t(0.35 * p[0] + 0.65 * 255);
      p[1] = uint8_t(0.35 * p[1] + 0.65 * 230);
      p[2] = uint8_t(0.35 * p[2]);
    };
    auto segment = [&](const float* pa, const float* pb) {
      Vec4 a = mul(M, {pa[0], pa[1], pa[2], 1.0}), b = mul(M, {pb[0], pb[1], pb[2], 1.0});
      if (a[2] < 0 && b[2] < 0) return;  // behind the near plane (z >= 0 is in front)
      if (a[2] < 0 || b[2] < 0) {
        const double t = a[2] / (a[2] - b[2]);
        Vec4 m{};
        for (int k = 0; k < 4; ++k) m[k] = a[k] + t * (b[k] - a[k]);
        (a[2] < 0 ? a : b) = m;
      }
      if (a[3] <= 0 || b[3] <= 0) return;
      const double ax = ox + sx * a[0] / a[3], ay = oy + sy * a[1] / a[3], az = a[2] / a[3];
      const double bx = ox + sx * b[0] / b[3], by = oy + sy * b[1] / b[3], bz = b[2] / b[3];
      if ((ax < 0 && bx < 0) || (ay < 0 && by < 0) || (ax > W && bx > W) || (ay > H && by > H))
        return;
      const double length = std::max(std::fabs(bx - ax), std::fabs(by - ay));
      if (length < 2.0 || length > 4.0 * W) return;
      ++drawn_edges;
      const int steps = int(std::ceil(length));
      for (int s = 0; s <= steps; ++s) {
        const double t = double(s) / steps;
        // z/w is affine in screen space along the projected segment.
        plot(int(std::floor(ax + t * (bx - ax))), int(std::floor(ay + t * (by - ay))),
             az + t * (bz - az));
      }
    };
    for (size_t t = 0; t + 2 < scene.vertices.size(); t += 3) {
      const float* p0 = scene.vertices[t].position;
      const float* p1 = scene.vertices[t + 1].position;
      const float* p2 = scene.vertices[t + 2].position;
      segment(p0, p1);
      segment(p1, p2);
      segment(p2, p0);
    }
    write_png(out_dir / ("overlay-triangles-" + stem + ".png"), W, H, 3, wire);
    std::printf("  wrote overlay-edges-%s.png and overlay-triangles-%s.png (%zu triangle edges "
                "at least 2 px long)\n",
                stem.c_str(), stem.c_str(), drawn_edges);
  }
  std::printf("wrote %s (world-colour.png, world-depth.png, world-depth.pfm)\n",
              out_dir.string().c_str());
  if (validation && renderer.validation_messages()) {
    std::printf("validation messages: %u\n", renderer.validation_messages());
    status = 3;
  }
  return status;
}

}  // namespace carview
