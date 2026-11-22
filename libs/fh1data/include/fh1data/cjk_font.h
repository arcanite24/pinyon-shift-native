// FH1 CJK bitmap fonts (media/ui/fonts/{CHTB,CHSB,JPB,KORB}.{abc,sbm} + .ini):
// - .abc: 5-byte header [ver_maj=1, ver_min=1, cell_size=48, bpp=4, ascent=42]
//   followed by a zlib stream decompressing to 65,535 x 8-byte big-endian glyph
//   metrics records indexed by (code_point - 1) for U+0001..U+FFFF:
//     u32be sbm_offset (byte offset into .sbm; 0 = missing/fallback glyph)
//     i8 pad, i8 a_bearing, u8 b_width, i8 c_bearing (advance = a + b + c)
// - .sbm: concatenated per-glyph zlib streams, each decompressing to
//   cell_size * cell_size / 2 bytes (4bpp, low nibble first: pixel 2k = byte & 0xF,
//   pixel 2k+1 = byte >> 4), scaled 0..15 -> 0..255 coverage.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh1 {

struct CjkGlyphMetrics {
  uint32_t sbm_offset = 0;
  int8_t a_bearing = 0;
  uint8_t b_width = 0;
  int8_t c_bearing = 0;
  int advance() const { return int(a_bearing) + int(b_width) + int(c_bearing); }
};

class CjkFont {
 public:
  // Loads `<stem>.abc` and `<stem>.sbm` from memory buffers.
  bool parse(std::span<const uint8_t> abc_bytes, std::vector<uint8_t> sbm_bytes, std::string* error = nullptr);
  // Loads the matching CJK bitmap font for a language code (`JP` -> JPB,
  // `KO` -> KORB, `CHT` / `CHS` -> CHTB / CHSB) from `game_root/media/ui/fonts/`.
  bool load_for_language(const std::filesystem::path& game_root, std::string_view language,
                         std::string* error = nullptr);

  // Adds a synthetic glyph (for unit tests without disc font files).
  void set_synthetic_glyph(uint32_t code_point, int cell_size, int ascent, int a, int b, int c,
                           std::vector<uint8_t> coverage_8bit);

  bool valid() const { return cell_size_ > 0 && (!entries_.empty() || !synthetic_.empty()); }
  int cell_size() const { return cell_size_; }
  int bpp() const { return bpp_; }
  int ascent() const { return ascent_; }
  float ea_scale() const { return ea_scale_; }
  float baseline_shift() const { return baseline_shift_; }
  const std::string& font_name() const { return name_; }

  // Returns true if the font has a non-fallback glyph for `code_point`.
  bool has_glyph(uint32_t code_point) const;
  // Returns the metrics record for `code_point` (nullptr if absent).
  const CjkGlyphMetrics* metrics(uint32_t code_point) const;

  // Decodes `code_point` into a `cell_size x cell_size` 8-bit coverage bitmap
  // (row-major, 0..255). Returns false if the code point is absent or corrupt.
  bool decode_glyph(uint32_t code_point, std::vector<uint8_t>& out_coverage, int& out_w, int& out_h) const;

  // Rasterizes `code_point` scaled to `target_px` (cell height in pixels) using
  // area/bilinear resampling into `out_coverage` (`out_w x out_h`), plus advance in px.
  bool rasterize_scaled(uint32_t code_point, int target_px, std::vector<uint8_t>& out_coverage, int& out_w,
                        int& out_h, float& out_advance) const;

  // Returns true if `language` uses a CJK bitmap font (JP, KO, CHT, CHS).
  static bool language_needs_cjk(std::string_view language);

 private:
  std::string name_;
  int cell_size_ = 0;
  int bpp_ = 4;
  int ascent_ = 42;
  float ea_scale_ = 0.65f;
  float baseline_shift_ = 0.05f;
  std::vector<CjkGlyphMetrics> entries_;  // indexed by (code_point - 1)
  std::vector<uint8_t> sbm_;
  struct SyntheticGlyph {
    CjkGlyphMetrics metrics{};
    std::vector<uint8_t> coverage;
  };
  std::unordered_map<uint32_t, SyntheticGlyph> synthetic_;
};

}  // namespace fh1
