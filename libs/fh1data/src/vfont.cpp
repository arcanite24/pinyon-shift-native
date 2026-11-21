#include "fh1data/vfont.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fh1 {
namespace {

uint32_t be32(std::span<const uint8_t> b, size_t at) {
  return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
}
uint16_t be16(std::span<const uint8_t> b, size_t at) { return uint16_t(b[at] << 8 | b[at + 1]); }
float bef(std::span<const uint8_t> b, size_t at) {
  const uint32_t u = be32(b, at);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
bool fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

}  // namespace

const VFontGlyph* VFont::find(uint32_t code_point) const {
  auto it = glyphs.find(code_point);
  return it == glyphs.end() ? nullptr : &it->second;
}

float VFont::cap_height() const {
  for (uint32_t c : {uint32_t('H'), uint32_t('0')})
    if (const VFontGlyph* g = find(c)) {
      float top = 0;
      for (const auto& v : g->vertices)
        if (v.sign > 0) top = std::max(top, v.y);
      if (top > 0) return top;
    }
  return 0.7f;
}

bool parse_vfont(std::span<const uint8_t> b, VFont& out, std::string* error) {
  out = VFont{};
  if (b.size() < 0x190 || std::memcmp(b.data(), "CAFF", 4) != 0) return fail(error, "not CAFF");
  const size_t data_size = be32(b, 0x44), data = be32(b, 0x48);
  const size_t gpu = data + be32(b, 0x60);
  if (data + data_size > b.size() || gpu > b.size()) return fail(error, "sections out of range");
  const auto d = b.subspan(data, data_size);
  if (d.size() < 0x370 || std::memcmp(d.data(), "vfont", 5) != 0) return fail(error, "not a vfont");
  const size_t count = be32(d, 0x34C);
  const size_t slots = be32(d, 0x360);
  const size_t table = be32(d, 0x35C);
  const size_t after = table + 4 * slots;
  if (after + 8 > d.size()) return fail(error, "code-point table out of range");
  const size_t m = be32(d, after), r = be32(d, after + 4);
  if (m + 72 > d.size() || r + 40 * count > d.size()) return fail(error, "metrics or records out of range");
  const size_t vertex_count = be32(d, m + 12), index_count = be32(d, m + 16);
  out.space_width = bef(d, m + 20);
  out.baseline_offset = bef(d, m + 28);
  out.ascent = be32(d, m + 36);
  out.descent = be32(d, m + 40);
  out.em_size = std::max<uint32_t>(1, be32(d, m + 68));
  const size_t vbytes = 8 * vertex_count, ibase = gpu + vbytes;
  if (ibase + 2 * index_count > b.size()) return fail(error, "mesh buffers out of range");
  for (size_t i = 0; i < count; ++i) {
    const size_t at = r + 40 * i;
    VFontGlyph g;
    g.code_point = be32(d, at);
    g.advance = bef(d, at + 4);
    const size_t first_vertex = be32(d, at + 12) / 8, nv = be32(d, at + 16);
    const size_t first_index = be32(d, at + 20), nt = be32(d, at + 24);
    const float offset = bef(d, at + 28);
    if (first_vertex + nv > vertex_count || first_index + 3 * nt > index_count)
      return fail(error, "glyph mesh out of range");
    g.vertices.resize(nv);
    bool first = true;
    for (size_t k = 0; k < nv; ++k) {
      const size_t va = gpu + 8 * (first_vertex + k);
      const uint16_t hx = be16(b, va);
      VFontVertex& v = g.vertices[k];
      v.sign = (hx & 0x8000) ? -1.0f : 1.0f;  // the sign bit, not x < 0 (a negative zero is a skirt)
      v.x = std::fabs(half_to_float(hx)) + offset;
      v.y = half_to_float(be16(b, va + 2));
      v.u = half_to_float(be16(b, va + 4));
      v.v = half_to_float(be16(b, va + 6));
      if (first) g.min_x = g.max_x = v.x, g.min_y = g.max_y = v.y, first = false;
      g.min_x = std::min(g.min_x, v.x), g.max_x = std::max(g.max_x, v.x);
      g.min_y = std::min(g.min_y, v.y), g.max_y = std::max(g.max_y, v.y);
    }
    g.indices.resize(3 * nt);
    for (size_t k = 0; k < 3 * nt; ++k) {
      g.indices[k] = be16(b, ibase + 2 * (first_index + k));
      if (g.indices[k] >= nv) return fail(error, "index past the glyph's vertices");
    }
    out.glyphs[g.code_point] = std::move(g);
  }
  return true;
}

float vfont_coverage(float u, float v, float dudx, float dudy, float dvdx, float dvdy, float sign) {
  const float g = sign * (u * u - v);
  const float gx = sign * (2 * u * dudx - dvdx), gy = sign * (2 * u * dudy - dvdy);
  const float len = std::sqrt(gx * gx + gy * gy);
  if (len < 1e-6f) return g < 0 ? 1.0f : 0.0f;
  return std::clamp(0.5f - g / len, 0.0f, 1.0f);
}

void rasterize_vfont_glyph(const VFontGlyph& glyph, float k, float pen_x, float pen_y, std::vector<uint8_t>& out,
                           int w, int h) {
  out.assign(size_t(w) * size_t(h), 0);
  std::vector<float> acc(size_t(w) * size_t(h), 0.0f);
  for (size_t t = 0; t + 2 < glyph.indices.size(); t += 3) {
    const VFontVertex* v[3] = {&glyph.vertices[glyph.indices[t]], &glyph.vertices[glyph.indices[t + 1]],
                               &glyph.vertices[glyph.indices[t + 2]]};
    float px[3], py[3];
    for (int i = 0; i < 3; ++i) px[i] = pen_x + v[i]->x * k, py[i] = pen_y - v[i]->y * k;
    const float e1x = px[1] - px[0], e1y = py[1] - py[0], e2x = px[2] - px[0], e2y = py[2] - py[0];
    const float det = e1x * e2y - e1y * e2x;
    if (std::fabs(det) < 1e-12f) continue;
    // Gradients of u and v in pixels: constant over the triangle.
    auto grad = [&](float a0, float a1, float a2, float& gx, float& gy) {
      gx = ((a1 - a0) * e2y - (a2 - a0) * e1y) / det;
      gy = ((a2 - a0) * e1x - (a1 - a0) * e2x) / det;
    };
    float dudx, dudy, dvdx, dvdy;
    grad(v[0]->u, v[1]->u, v[2]->u, dudx, dudy);
    grad(v[0]->v, v[1]->v, v[2]->v, dvdx, dvdy);
    const float sign = v[0]->sign;
    const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]}))));
    const int x1 = std::min(w - 1, int(std::ceil(std::max({px[0], px[1], px[2]}))));
    const int y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]}))));
    const int y1 = std::min(h - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float cx = float(x) + 0.5f - px[0], cy = float(y) + 0.5f - py[0];
        // Barycentrics of the pixel centre (b1 along e1, b2 along e2).
        const float b1 = (cx * e2y - cy * e2x) / det, b2 = (e1x * cy - e1y * cx) / det;
        const float eps = -1e-5f;
        if (b1 < eps || b2 < eps || b1 + b2 > 1 - eps) continue;
        const float u = v[0]->u + (v[1]->u - v[0]->u) * b1 + (v[2]->u - v[0]->u) * b2;
        const float vv = v[0]->v + (v[1]->v - v[0]->v) * b1 + (v[2]->v - v[0]->v) * b2;
        const float c = vfont_coverage(u, vv, dudx, dudy, dvdx, dvdy, sign);
        float& a = acc[size_t(y) * size_t(w) + size_t(x)];
        a = a + c * (1 - a);  // source over
      }
  }
  for (size_t i = 0; i < acc.size(); ++i) out[i] = uint8_t(std::lround(std::clamp(acc[i], 0.0f, 1.0f) * 255));
}

}  // namespace fh1
