// FH1 UI vector fonts (`media/ui/Fonts.zip` members `*_vector_aa.dt`): a
// CAFF container whose `.data` section holds the metrics and 40-byte glyph
// records and whose `.gpu` section holds one vertex buffer (four big-endian
// half floats x, y, u, v) and one u16 index buffer shared by every glyph.
// A glyph is a triangle list in em units, y up from the baseline; the sign
// of x flags the outer anti-aliasing skirt, and (u, v) are curve
// coordinates whose implicit u^2 - v gives each pixel's coverage. No
// kerning.
// Spec: FORMAT-FONTS (docs/formats/FONTS.md) in horizon1-recomp.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "fh1data/placements.h"  // half_to_float

namespace fh1 {

struct VFontVertex {
  float x = 0, y = 0;  // em, from the pen (the record's offset applied), y up
  float u = 0, v = 0;
  float sign = 1;      // +1 inside the outline, -1 the outer skirt
};

struct VFontGlyph {
  uint32_t code_point = 0;
  float advance = 0;  // em
  std::vector<VFontVertex> vertices;
  std::vector<uint16_t> indices;  // a triangle list into vertices
  float min_x = 0, min_y = 0, max_x = 0, max_y = 0;  // the mesh's box, em
};

struct VFont {
  float space_width = 0.25f;  // em
  float baseline_offset = 0;  // em
  uint32_t ascent = 0, descent = 0, em_size = 1;  // font units (Windows ascent, descent)
  std::unordered_map<uint32_t, VFontGlyph> glyphs;  // by code point
  const VFontGlyph* find(uint32_t code_point) const;
  // The top of the inside triangles of `H` (or of `0`), em: the cap height.
  float cap_height() const;
};

bool parse_vfont(std::span<const uint8_t> bytes, VFont& out, std::string* error);

// A glyph's coverage at `px_per_em`, by the spec's rule, into a w x h
// 8-bit bitmap (row-major, y down). The glyph's pen sits at (pen_x, pen_y)
// in the bitmap's pixels (pen_y on the baseline). Triangles composite
// source-over.
void rasterize_vfont_glyph(const VFontGlyph& glyph, float px_per_em, float pen_x, float pen_y,
                           std::vector<uint8_t>& coverage, int w, int h);

// The rule for one pixel: (u, v) at the pixel, their gradients in pixels,
// and the sign.
float vfont_coverage(float u, float v, float dudx, float dudy, float dvdx, float dvdy, float sign);

}  // namespace fh1
