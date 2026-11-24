#include "cars/livery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "cars/util.h"

namespace cars {
namespace {

constexpr float kPi = 3.14159265358979323846f;

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1])); }
float bef32(const uint8_t* p) {
  const uint32_t u = be32(p);
  float f = 0.0f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Built-in vector primitives for standalone / fallback shapes.
VinylMesh make_builtin_shape(const std::string& id) {
  VinylMesh m;
  m.name = id;
  m.min_x = -0.5f;
  m.min_y = -0.5f;
  m.max_x = 0.5f;
  m.max_y = 0.5f;
  if (id == "@circle") {
    constexpr int kSegs = 40;
    m.vertices.push_back({0.0f, 0.0f, 0xFFFFFFFFu});
    for (int i = 0; i < kSegs; ++i) {
      const float a = 2.0f * kPi * float(i) / float(kSegs);
      m.vertices.push_back({0.5f * std::cos(a), 0.5f * std::sin(a), 0xFFFFFFFFu});
    }
    for (int i = 0; i < kSegs; ++i) {
      m.indices.push_back(0);
      m.indices.push_back(uint32_t(1 + i));
      m.indices.push_back(uint32_t(1 + (i + 1) % kSegs));
    }
  } else if (id == "@star") {
    constexpr int kPts = 10;
    m.vertices.push_back({0.0f, 0.0f, 0xFFFFFFFFu});
    for (int i = 0; i < kPts; ++i) {
      const float a = -0.5f * kPi + 2.0f * kPi * float(i) / float(kPts);
      const float r = (i % 2 == 0) ? 0.5f : 0.21f;
      m.vertices.push_back({r * std::cos(a), r * std::sin(a), 0xFFFFFFFFu});
    }
    for (int i = 0; i < kPts; ++i) {
      m.indices.push_back(0);
      m.indices.push_back(uint32_t(1 + i));
      m.indices.push_back(uint32_t(1 + (i + 1) % kPts));
    }
  } else if (id == "@chevron") {
    m.vertices = {{-0.5f, 0.1f, 0xFFFFFFFFu},  {0.0f, -0.45f, 0xFFFFFFFFu}, {0.5f, 0.1f, 0xFFFFFFFFu},
                  {0.5f, 0.45f, 0xFFFFFFFFu},  {0.0f, -0.10f, 0xFFFFFFFFu}, {-0.5f, 0.45f, 0xFFFFFFFFu}};
    m.indices = {0, 1, 4, 0, 4, 5, 1, 2, 3, 1, 3, 4};
  } else {
    // @rect / @stripe default quad.
    m.vertices = {{-0.5f, -0.5f, 0xFFFFFFFFu}, {0.5f, -0.5f, 0xFFFFFFFFu},
                  {0.5f, 0.5f, 0xFFFFFFFFu},   {-0.5f, 0.5f, 0xFFFFFFFFu}};
    m.indices = {0, 1, 2, 0, 2, 3};
  }
  return m;
}

// Rasterizes one triangle into a single-layer RGBA coverage buffer using clamped
// additive coverage (capped at 255) so adjacent triangles sharing an internal edge
// sum cleanly to full coverage (0.5 + 0.5 = 1.0) with zero diagonal seams and no
// double-blending when the layer's alpha is applied once in the composite pass.
void rasterize_triangle_layer(std::vector<uint8_t>& layer_rgba, uint32_t w, uint32_t h, float x0, float y0, float x1,
                              float y1, float x2, float y2, uint8_t cr, uint8_t cg, uint8_t cb, float vert_alpha,
                              int& dirty_x0, int& dirty_y0, int& dirty_x1, int& dirty_y1) {
  const float min_xf = std::floor(std::min({x0, x1, x2}));
  const float max_xf = std::ceil(std::max({x0, x1, x2}));
  const float min_yf = std::floor(std::min({y0, y1, y2}));
  const float max_yf = std::ceil(std::max({y0, y1, y2}));
  const int ix0 = std::clamp(int(min_xf), 0, int(w) - 1);
  const int ix1 = std::clamp(int(max_xf), 0, int(w) - 1);
  const int iy0 = std::clamp(int(min_yf), 0, int(h) - 1);
  const int iy1 = std::clamp(int(max_yf), 0, int(h) - 1);
  if (ix0 > ix1 || iy0 > iy1) return;

  const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
  if (std::fabs(area) < 1e-4f) return;
  const float sign = area > 0.0f ? 1.0f : -1.0f;

  auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
  };
  const float e0_len = std::max(1e-4f, std::hypot(x1 - x0, y1 - y0));
  const float e1_len = std::max(1e-4f, std::hypot(x2 - x1, y2 - y1));
  const float e2_len = std::max(1e-4f, std::hypot(x0 - x2, y0 - y2));

  dirty_x0 = std::min(dirty_x0, ix0);
  dirty_y0 = std::min(dirty_y0, iy0);
  dirty_x1 = std::max(dirty_x1, ix1);
  dirty_y1 = std::max(dirty_y1, iy1);

  for (int py = iy0; py <= iy1; ++py) {
    const float fy = float(py) + 0.5f;
    for (int px = ix0; px <= ix1; ++px) {
      const float fx = float(px) + 0.5f;
      const float d0 = sign * edge(x0, y0, x1, y1, fx, fy) / e0_len;
      const float d1 = sign * edge(x1, y1, x2, y2, fx, fy) / e1_len;
      const float d2 = sign * edge(x2, y2, x0, y0, fx, fy) / e2_len;
      const float dmin = std::min({d0, d1, d2});
      if (dmin < -0.6f) continue;
      const float cov = std::clamp(dmin + 0.5f, 0.0f, 1.0f);
      const int add_u8 = int(std::clamp(std::lround(vert_alpha * cov * 255.0f), 0l, 255l));
      if (add_u8 == 0) continue;

      uint8_t* dst = &layer_rgba[(size_t(py) * w + size_t(px)) * 4u];
      const int prev = int(dst[3]);
      const int next = std::min(255, prev + add_u8);
      if (add_u8 >= prev) {
        dst[0] = cr;
        dst[1] = cg;
        dst[2] = cb;
      }
      dst[3] = uint8_t(next);
    }
  }
}

}  // namespace

const char* livery_plane_name(LiveryPlaneKind p) {
  switch (p) {
    case LiveryPlaneKind::kFullAtlas: return "FULL BODY";
    case LiveryPlaneKind::kTop: return "TOP / HOOD";
    case LiveryPlaneKind::kLeft: return "LEFT SIDE";
    case LiveryPlaneKind::kRight: return "RIGHT SIDE";
    case LiveryPlaneKind::kFront: return "FRONT";
    case LiveryPlaneKind::kRear: return "REAR";
    case LiveryPlaneKind::kWing: return "REAR WING";
    default: return "FULL BODY";
  }
}

bool parse_vinyl_carbin(const std::vector<uint8_t>& data, VinylMesh& out, std::string* error) {
  out = VinylMesh{};
  if (data.size() < 160) {
    if (error) *error = "vinyl carbin too small";
    return false;
  }
  const uint8_t* b = data.data();
  const size_t n = data.size();
  const uint32_t kind = be32(b);
  if (kind != 1 && kind != 3) {
    if (error) *error = "unsupported vinyl carbin kind " + std::to_string(kind);
    return false;
  }
  out.kind = kind;
  out.min_x = bef32(b + 4);
  out.min_y = bef32(b + 8);
  out.max_x = bef32(b + 20);
  out.max_y = bef32(b + 24);

  // Locate the mesh header: name_len (1..64), ASCII name, vertex_format (2 or 3),
  // vertex_count (>0), stride (36). Works uniformly for both kind-3 (tag 2,
  // offset ~120) and kind-1 (tag 1, offset ~112) shapes across all 1,372 files.
  size_t vb_off = 0;
  uint32_t vcount = 0;
  uint32_t stride = 0;
  for (size_t off = 96; off + 20 < std::min<size_t>(n, 240); off += 4) {
    const uint32_t nlen = be32(b + off);
    if (nlen == 0 || nlen > 64 || off + 4 + nlen + 12 >= n) continue;
    bool ascii = true;
    for (uint32_t k = 0; k < nlen; ++k) {
      const uint8_t ch = b[off + 4 + k];
      if (ch < 0x20 || ch > 0x7E) { ascii = false; break; }
    }
    if (!ascii) continue;
    const size_t h = off + 4 + nlen;
    const uint32_t vfmt = be32(b + h);
    const uint32_t vc = be32(b + h + 4);
    const uint32_t st = be32(b + h + 8);
    if ((vfmt == 2 || vfmt == 3) && vc >= 3 && vc < 500000 && (st == 36 || st == 24 || st == 32)) {
      out.name.assign(reinterpret_cast<const char*>(b + off + 4), nlen);
      vcount = vc;
      stride = st;
      vb_off = h + 12;
      break;
    }
  }
  if (vb_off == 0 || vcount < 3) {
    if (error) *error = "vinyl carbin: mesh vertex header not found";
    return false;
  }

  // Kind-1 vinyls prefix each 36-byte vertex record with a 4-byte record id (0x00000001).
  const size_t rec_stride = (kind == 1 && vb_off + 4 < n && be32(b + vb_off) == 1) ? (stride + 4) : stride;
  const size_t xy_off = (rec_stride == stride + 4) ? 4 : 0;
  if (vb_off + size_t(vcount) * rec_stride > n) {
    if (error) *error = "vinyl carbin: truncated vertex buffer";
    return false;
  }

  out.vertices.resize(vcount);
  bool non_white = false;
  float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
  for (uint32_t i = 0; i < vcount; ++i) {
    const uint8_t* v = b + vb_off + size_t(i) * rec_stride + xy_off;
    VinylVertex vv;
    vv.x = bef32(v + 0);
    vv.y = bef32(v + 4);
    vv.argb = stride >= 36 ? be32(v + 32) : 0xFFFFFFFFu;
    if ((vv.argb & 0xFFFFFFu) != 0xFFFFFFu) non_white = true;
    lo_x = std::min(lo_x, vv.x);
    lo_y = std::min(lo_y, vv.y);
    hi_x = std::max(hi_x, vv.x);
    hi_y = std::max(hi_y, vv.y);
    out.vertices[i] = vv;
  }
  out.is_decal = non_white;
  if (hi_x > lo_x && hi_y > lo_y) {
    out.min_x = lo_x;
    out.min_y = lo_y;
    out.max_x = hi_x;
    out.max_y = hi_y;
  }

  // Scan after vertex buffer for one or more index buffers:
  // (prim_type in {3=strip, 4=list}, index_count >= 3, index_stride in {2, 4}, indices...).
  size_t cursor = vb_off + size_t(vcount) * rec_stride;
  while (cursor + 12 < n) {
    const uint32_t prim = be32(b + cursor);
    const uint32_t icount = be32(b + cursor + 4);
    const uint32_t istride = be32(b + cursor + 8);
    if ((prim == 3 || prim == 4) && (istride == 2 || istride == 4) && icount >= 3 && icount <= 1000000 &&
        cursor + 12 + size_t(icount) * istride <= n) {
      // Validate first few indices are in range [0, vcount).
      bool ok = true;
      const size_t check_n = std::min<size_t>(icount, 12);
      for (size_t k = 0; k < check_n; ++k) {
        const uint32_t idx = istride == 2 ? be16(b + cursor + 12 + k * 2) : be32(b + cursor + 12 + k * 4);
        if (idx >= vcount && idx != 0xFFFFu && idx != 0xFFFFFFFFu) { ok = false; break; }
      }
      if (ok) {
        std::vector<uint32_t> raw(icount);
        for (uint32_t k = 0; k < icount; ++k) {
          raw[k] = istride == 2 ? be16(b + cursor + 12 + size_t(k) * 2)
                                : be32(b + cursor + 12 + size_t(k) * 4);
        }
        if (prim == 4) {
          for (uint32_t k = 0; k + 2 < icount; k += 3) {
            if (raw[k] < vcount && raw[k + 1] < vcount && raw[k + 2] < vcount) {
              out.indices.push_back(raw[k]);
              out.indices.push_back(raw[k + 1]);
              out.indices.push_back(raw[k + 2]);
            }
          }
        } else {
          // Triangle strip (prim == 3) with primitive restart support (0xFFFF).
          for (uint32_t k = 0; k + 2 < icount; ++k) {
            const uint32_t i0 = raw[k], i1 = raw[k + 1], i2 = raw[k + 2];
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
            if (i0 == i1 || i1 == i2 || i0 == i2) continue;
            if (k % 2 == 0) {
              out.indices.push_back(i0);
              out.indices.push_back(i1);
              out.indices.push_back(i2);
            } else {
              out.indices.push_back(i1);
              out.indices.push_back(i0);
              out.indices.push_back(i2);
            }
          }
        }
        cursor += 12 + size_t(icount) * istride;
        continue;
      }
    }
    cursor += 1;
  }

  // Fallback if no separate index block was located: interpret vertices sequentially as triangles.
  if (out.indices.empty() && vcount >= 3) {
    for (uint32_t i = 0; i + 2 < vcount; i += 3) {
      out.indices.push_back(i);
      out.indices.push_back(i + 1);
      out.indices.push_back(i + 2);
    }
  }
  return !out.indices.empty();
}

bool LiveryLibrary::open(const std::filesystem::path& livery_zip, std::string* error) {
  archive_.reset();
  vinyls_.clear();
  decals_.clear();
  cache_.clear();
  archive_ = fh1::Archive::open(livery_zip, error);
  if (!archive_) return false;
  for (const auto& m : archive_->members()) {
    const std::string ln = lower(m.name);
    if (ln.rfind("vinyls/", 0) == 0 && ln.size() > 7 && ln.substr(ln.size() - 7) == ".carbin") {
      vinyls_.push_back(m.name);
    } else if (ln.rfind("decals/", 0) == 0 && ln.size() > 7 && ln.substr(ln.size() - 7) == ".carbin") {
      decals_.push_back(m.name);
    }
  }
  std::sort(vinyls_.begin(), vinyls_.end());
  std::sort(decals_.begin(), decals_.end());
  return !vinyls_.empty() || !decals_.empty();
}

const VinylMesh* LiveryLibrary::load_shape(const std::string& member_path, std::string* error) {
  if (auto it = cache_.find(member_path); it != cache_.end()) return &it->second;
  if (!archive_) {
    if (error) *error = "Livery.zip not open";
    return nullptr;
  }
  auto r = archive_->read(member_path);
  if (!r.ok()) {
    if (error) *error = member_path + ": " + r.error;
    return nullptr;
  }
  VinylMesh mesh;
  if (!parse_vinyl_carbin(r.data, mesh, error)) return nullptr;
  auto [ins, _] = cache_.emplace(member_path, std::move(mesh));
  return &ins->second;
}

bool composite_livery(const LiveryDocument& doc, LiveryLibrary* lib, const std::vector<bake::LiveryPlane>* planes,
                      std::vector<uint8_t>& out_rgba, uint32_t size) {
  if (size == 0 || size > 4096) return false;
  out_rgba.assign(size_t(size) * size * 4u, 0u);

  // Determine UV sub-rectangle + plane rotation using Masks.xml planes if available.
  struct PlaneRegion {
    float u0 = 0.0f, v0 = 0.0f, w = 1.0f, h = 1.0f, rot_deg = 0.0f;
  };
  auto plane_region = [&](LiveryPlaneKind pk) -> PlaneRegion {
    if (planes && !planes->empty() && pk != LiveryPlaneKind::kFullAtlas) {
      const char* want = pk == LiveryPlaneKind::kTop     ? "top"
                         : pk == LiveryPlaneKind::kLeft  ? "left"
                         : pk == LiveryPlaneKind::kRight ? "right"
                         : pk == LiveryPlaneKind::kFront ? "front"
                         : pk == LiveryPlaneKind::kWing  ? "wing"
                                                         : "back";
      for (const auto& lp : *planes) {
        if (lp.valid && lower(lp.name).find(want) != std::string::npos) {
          const float u0 = std::clamp((512.0f + lp.left) / 1024.0f, 0.0f, 1.0f);
          const float u1 = std::clamp((512.0f + lp.right) / 1024.0f, 0.0f, 1.0f);
          const float v0 = std::clamp((512.0f + lp.top) / 1024.0f, 0.0f, 1.0f);
          const float v1 = std::clamp((512.0f + lp.bottom) / 1024.0f, 0.0f, 1.0f);
          const float w = std::fabs(u1 - u0);
          const float h = std::fabs(v1 - v0);
          if (w > 0.01f && h > 0.01f) {
            return {std::min(u0, u1), std::min(v0, v1), w, h, lp.rotation};
          }
        }
      }
    }
    return {};
  };

  std::vector<uint8_t> scratch(size_t(size) * size * 4u, 0u);
  for (const VinylLayer& layer : doc.layers) {
    if (!layer.visible || layer.alpha <= 1e-4f) continue;
    VinylMesh builtin;
    const VinylMesh* mesh = nullptr;
    if (!layer.shape_path.empty() && layer.shape_path[0] == '@') {
      builtin = make_builtin_shape(layer.shape_path);
      mesh = &builtin;
    } else if (lib && lib->is_open()) {
      mesh = lib->load_shape(layer.shape_path, nullptr);
    }
    if (!mesh) {
      builtin = make_builtin_shape("@rect");
      mesh = &builtin;
    }

    const float cx_m = 0.5f * (mesh->min_x + mesh->max_x);
    const float cy_m = 0.5f * (mesh->min_y + mesh->max_y);
    const float span_x = std::max(1e-4f, mesh->max_x - mesh->min_x);
    const float span_y = std::max(1e-4f, mesh->max_y - mesh->min_y);
    const PlaneRegion pr = plane_region(layer.plane);
    const float rad = (layer.rot_deg + pr.rot_deg) * (kPi / 180.0f);
    const float cr = std::cos(rad), sr = std::sin(rad);
    const float fx = layer.flip_x ? -1.0f : 1.0f;
    const float fy = layer.flip_y ? -1.0f : 1.0f;

    const uint8_t lr = uint8_t((layer.rgb >> 16) & 0xFF);
    const uint8_t lg = uint8_t((layer.rgb >> 8) & 0xFF);
    const uint8_t lb = uint8_t(layer.rgb & 0xFF);

    auto xform = [&](const VinylVertex& v) -> std::array<float, 2> {
      float nx = fx * ((v.x - cx_m) / span_x);
      float ny = fy * ((v.y - cy_m) / span_y);
      nx += layer.skew * ny;
      const float sx = nx * layer.scale_x;
      const float sy = ny * layer.scale_y;
      const float rx = cr * sx - sr * sy;
      const float ry = sr * sx + cr * sy;
      const float u = pr.u0 + (layer.x + rx) * pr.w;
      const float vv = pr.v0 + (layer.y + ry) * pr.h;
      return {u * float(size), vv * float(size)};
    };

    int dx0 = int(size), dy0 = int(size), dx1 = -1, dy1 = -1;
    for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
      const VinylVertex& v0 = mesh->vertices[mesh->indices[i]];
      const VinylVertex& v1 = mesh->vertices[mesh->indices[i + 1]];
      const VinylVertex& v2 = mesh->vertices[mesh->indices[i + 2]];
      const auto p0 = xform(v0);
      const auto p1 = xform(v1);
      const auto p2 = xform(v2);

      // Multi-colour decals preserve per-vertex artwork colour; single-colour vinyls tint by layer.rgb.
      uint8_t r = lr, g = lg, b = lb;
      float vert_a = 1.0f;
      if (mesh->is_decal) {
        const uint32_t c0 = v0.argb;
        const uint8_t vr = uint8_t((c0 >> 16) & 0xFF);
        const uint8_t vg = uint8_t((c0 >> 8) & 0xFF);
        const uint8_t vb = uint8_t(c0 & 0xFF);
        const uint8_t va = uint8_t((c0 >> 24) & 0xFF);
        r = uint8_t((uint32_t(vr) * lr) / 255u);
        g = uint8_t((uint32_t(vg) * lg) / 255u);
        b = uint8_t((uint32_t(vb) * lb) / 255u);
        vert_a = float(va) / 255.0f;
      }
      rasterize_triangle_layer(scratch, size, size, p0[0], p0[1], p1[0], p1[1], p2[0], p2[1], r, g, b, vert_a, dx0,
                               dy0, dx1, dy1);
    }

    // Clamp compositing to the plane's UV sub-rectangle so plane-local layers never bleed across seams.
    if (dx0 <= dx1 && dy0 <= dy1) {
      const int clip_x0 = std::max(dx0, int(std::floor(pr.u0 * float(size))));
      const int clip_y0 = std::max(dy0, int(std::floor(pr.v0 * float(size))));
      const int clip_x1 = std::min(dx1, int(std::ceil((pr.u0 + pr.w) * float(size))) - 1);
      const int clip_y1 = std::min(dy1, int(std::ceil((pr.v0 + pr.h) * float(size))) - 1);
      for (int py = dy0; py <= dy1; ++py) {
        for (int px = dx0; px <= dx1; ++px) {
          uint8_t* src = &scratch[(size_t(py) * size + size_t(px)) * 4u];
          const uint8_t sa_u8 = src[3];
          if (sa_u8 > 0 && px >= clip_x0 && px <= clip_x1 && py >= clip_y0 && py <= clip_y1) {
            uint8_t* dst = &out_rgba[(size_t(py) * size + size_t(px)) * 4u];
            const float sa = (float(sa_u8) / 255.0f) * std::clamp(layer.alpha, 0.0f, 1.0f);
            if (layer.is_mask) {
              // Mask/Cutout punches through lower vinyl alpha back to base car paint.
              dst[3] = uint8_t(std::clamp(std::lround(float(dst[3]) * (1.0f - sa)), 0l, 255l));
            } else {
              const float da = float(dst[3]) / 255.0f;
              const float oa = sa + da * (1.0f - sa);
              if (oa > 1e-5f) {
                dst[0] = uint8_t(std::clamp((float(src[0]) * sa + float(dst[0]) * da * (1.0f - sa)) / oa, 0.0f, 255.0f));
                dst[1] = uint8_t(std::clamp((float(src[1]) * sa + float(dst[1]) * da * (1.0f - sa)) / oa, 0.0f, 255.0f));
                dst[2] = uint8_t(std::clamp((float(src[2]) * sa + float(dst[2]) * da * (1.0f - sa)) / oa, 0.0f, 255.0f));
                dst[3] = uint8_t(std::clamp(oa * 255.0f, 0.0f, 255.0f));
              }
            }
          }
          src[0] = src[1] = src[2] = src[3] = 0;
        }
      }
    }
  }
  return true;
}

bool save_livery_file(const std::filesystem::path& path, const LiveryDocument& doc, std::string* error) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = "cannot write " + path.string();
    return false;
  }
  auto write_region = [&](const char* tag, const PaintRegionSpec& r) {
    out << "region " << tag << " " << (r.active ? 1 : 0) << " " << int(r.finish) << " " << std::hex << r.rgb << " "
        << r.sec_rgb << std::dec << " " << r.two_tone_scale << " " << r.two_tone_bias << " " << r.two_tone_power
        << "\n";
  };
  out << "shift-livery 1\n";
  out << "car " << doc.car_id << "\n";
  out << "name " << doc.name << "\n";
  out << "tint " << doc.paint.window_tint << "\n";
  write_region("body", doc.paint.body);
  write_region("hood", doc.paint.hood);
  write_region("wing", doc.paint.wing);
  write_region("mirrors", doc.paint.mirrors);
  write_region("rims", doc.paint.rims);
  write_region("calipers", doc.paint.calipers);
  for (const auto& l : doc.layers) {
    out << "layer " << l.shape_path << " " << int(l.plane) << " " << l.x << " " << l.y << " " << l.scale_x << " "
        << l.scale_y << " " << l.rot_deg << " " << l.skew << " " << std::hex << l.rgb << std::dec << " " << l.alpha
        << " " << (l.flip_x ? 1 : 0) << " " << (l.flip_y ? 1 : 0) << " " << (l.visible ? 1 : 0) << " "
        << (l.is_mask ? 1 : 0) << " " << l.group_id << "\n";
  }
  return out.good();
}

bool load_livery_file(const std::filesystem::path& path, LiveryDocument& out, std::string* error) {
  out = LiveryDocument{};
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  std::string line;
  if (!std::getline(in, line)) {
    if (error) *error = "empty livery file";
    return false;
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line != "shift-livery 1") {
    // Graceful import of opaque FH1 binary .liv files: preserve filename as name
    // with default paint active so existing imported garage saves never fail.
    out.name = path.stem().string();
    return true;
  }
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ss(line);
    std::string tok;
    if (!(ss >> tok)) continue;
    if (tok == "car") {
      ss >> out.car_id;
    } else if (tok == "name") {
      std::getline(ss >> std::ws, out.name);
    } else if (tok == "tint") {
      ss >> out.paint.window_tint;
    } else if (tok == "region") {
      std::string tag;
      int act = 0, fin = 0;
      uint32_t rgb = 0, sec = 0;
      float sc = 0.25f, bi = 0.0f, pw = 4.0f;
      if (ss >> tag >> act >> fin >> std::hex >> rgb >> sec >> std::dec >> sc >> bi >> pw) {
        PaintRegionSpec* dst = tag == "body"       ? &out.paint.body
                               : tag == "hood"     ? &out.paint.hood
                               : tag == "wing"     ? &out.paint.wing
                               : tag == "mirrors"  ? &out.paint.mirrors
                               : tag == "rims"     ? &out.paint.rims
                               : tag == "calipers" ? &out.paint.calipers
                                                   : nullptr;
        if (dst) {
          dst->active = act != 0;
          dst->finish = PaintFinish(std::clamp(fin, 0, int(PaintFinish::kCount) - 1));
          dst->rgb = rgb;
          dst->sec_rgb = sec;
          dst->two_tone_scale = sc;
          dst->two_tone_bias = bi;
          dst->two_tone_power = pw;
        }
      }
    } else if (tok == "layer") {
      VinylLayer l;
      int pl = 0, fx = 0, fy = 0, vis = 1;
      if (ss >> l.shape_path >> pl >> l.x >> l.y >> l.scale_x >> l.scale_y >> l.rot_deg >> l.skew >> std::hex >>
          l.rgb >> std::dec >> l.alpha >> fx >> fy >> vis) {
        int mask = 0, gid = 0;
        if (ss >> mask) l.is_mask = mask != 0;
        if (ss >> gid) l.group_id = gid;
        l.plane = LiveryPlaneKind(std::clamp(pl, 0, int(LiveryPlaneKind::kCount) - 1));
        l.flip_x = fx != 0;
        l.flip_y = fy != 0;
        l.visible = vis != 0;
        out.layers.push_back(std::move(l));
      }
    }
  }
  return true;
}

}  // namespace cars
