#include "fh1data/cjk_font.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

#include "fh1data/inflate.h"

namespace fh1 {
namespace {

std::string upper_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool read_file_bytes(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}

// Inflates a standard zlib stream (2-byte CMF/FLG header + raw RFC 1951 deflate).
std::optional<std::vector<uint8_t>> inflate_zlib(std::span<const uint8_t> data, size_t expected_size) {
  if (data.size() < 6) return std::nullopt;
  const uint8_t cmf = data[0];
  const uint8_t flg = data[1];
  if ((cmf & 0x0F) != 8) return std::nullopt;  // deflate method required
  if (((uint16_t(cmf) << 8) | flg) % 31 != 0) return std::nullopt;
  size_t off = 2;
  if (flg & 0x20) off += 4;  // preset dictionary (unused here)
  if (off >= data.size()) return std::nullopt;
  return inflate_raw(data.subspan(off), expected_size);
}

}  // namespace

bool CjkFont::language_needs_cjk(std::string_view language) {
  const std::string l = upper_str(language);
  return l == "JP" || l == "KO" || l == "CHT" || l == "CHS";
}

bool CjkFont::parse(std::span<const uint8_t> abc_bytes, std::vector<uint8_t> sbm_bytes, std::string* error) {
  entries_.clear();
  sbm_.clear();
  synthetic_.clear();
  if (abc_bytes.size() < 12) {
    if (error) *error = "cjk .abc truncated";
    return false;
  }
  cell_size_ = int(abc_bytes[2]);
  bpp_ = int(abc_bytes[3]);
  ascent_ = int(abc_bytes[4]);
  if (cell_size_ <= 0 || cell_size_ > 128 || (bpp_ != 4 && bpp_ != 8)) {
    if (error) *error = "cjk .abc unsupported cell size or bpp";
    return false;
  }
  constexpr size_t kEntries = 65535;
  auto raw = inflate_zlib(abc_bytes.subspan(5), kEntries * 8);
  if (!raw || raw->size() != kEntries * 8) {
    if (error) *error = "cjk .abc zlib inflate failed";
    return false;
  }
  entries_.resize(kEntries);
  const uint8_t* p = raw->data();
  for (size_t i = 0; i < kEntries; ++i, p += 8) {
    CjkGlyphMetrics& m = entries_[i];
    m.sbm_offset = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
    m.a_bearing = static_cast<int8_t>(p[5]);
    m.b_width = p[6];
    m.c_bearing = static_cast<int8_t>(p[7]);
  }
  sbm_ = std::move(sbm_bytes);
  return true;
}

bool CjkFont::load_for_language(const std::filesystem::path& game_root, std::string_view language, std::string* error) {
  if (game_root.empty()) return false;
  const std::string l = upper_str(language);
  std::string stem;
  if (l == "JP") {
    stem = "JPB";
    ea_scale_ = 0.66f;
    baseline_shift_ = 0.074f;
  } else if (l == "KO") {
    stem = "KORB";
    ea_scale_ = 0.65f;
    baseline_shift_ = 0.048f;
  } else if (l == "CHT" || l == "CHS") {
    stem = (l == "CHS") ? "CHSB" : "CHTB";
    ea_scale_ = 0.61f;
    baseline_shift_ = 0.04f;
  } else {
    if (error) *error = "language does not require a CJK bitmap font";
    return false;
  }

  const auto dir = game_root / "media" / "ui" / "fonts";
  std::error_code ec;
  auto abc_path = dir / (stem + ".abc");
  auto sbm_path = dir / (stem + ".sbm");
  if (!std::filesystem::exists(abc_path, ec) && stem == "CHSB") {
    stem = "CHTB";
    abc_path = dir / "CHTB.abc";
    sbm_path = dir / "CHTB.sbm";
  }
  std::vector<uint8_t> abc_bytes, sbm_bytes;
  if (!read_file_bytes(abc_path, abc_bytes) || !read_file_bytes(sbm_path, sbm_bytes)) {
    if (error) *error = "cannot read " + abc_path.string() + " / .sbm";
    return false;
  }
  name_ = stem;
  return parse(abc_bytes, std::move(sbm_bytes), error);
}

void CjkFont::set_synthetic_glyph(uint32_t code_point, int cell_size, int ascent, int a, int b, int c,
                                  std::vector<uint8_t> coverage_8bit) {
  if (cell_size <= 0) return;
  cell_size_ = cell_size;
  ascent_ = ascent;
  bpp_ = 4;
  SyntheticGlyph sg;
  sg.metrics.sbm_offset = 1;
  sg.metrics.a_bearing = static_cast<int8_t>(a);
  sg.metrics.b_width = static_cast<uint8_t>(std::clamp(b, 0, 255));
  sg.metrics.c_bearing = static_cast<int8_t>(c);
  sg.coverage = std::move(coverage_8bit);
  synthetic_[code_point] = std::move(sg);
}

bool CjkFont::has_glyph(uint32_t code_point) const {
  if (synthetic_.count(code_point) > 0) return true;
  if (code_point == 0 || code_point > entries_.size()) return false;
  const auto& m = entries_[code_point - 1];
  return m.sbm_offset > 0 && m.sbm_offset < sbm_.size();
}

const CjkGlyphMetrics* CjkFont::metrics(uint32_t code_point) const {
  if (auto it = synthetic_.find(code_point); it != synthetic_.end()) return &it->second.metrics;
  if (code_point == 0 || code_point > entries_.size()) return nullptr;
  const auto& m = entries_[code_point - 1];
  if (m.sbm_offset == 0 || m.sbm_offset >= sbm_.size()) return nullptr;
  return &m;
}

bool CjkFont::decode_glyph(uint32_t code_point, std::vector<uint8_t>& out_coverage, int& out_w, int& out_h) const {
  if (auto it = synthetic_.find(code_point); it != synthetic_.end()) {
    out_w = cell_size_;
    out_h = cell_size_;
    out_coverage = it->second.coverage;
    return out_coverage.size() >= size_t(out_w) * size_t(out_h);
  }
  const CjkGlyphMetrics* m = metrics(code_point);
  if (!m || m->sbm_offset >= sbm_.size() || cell_size_ <= 0) return false;
  const size_t raw_bytes = size_t(cell_size_) * size_t(cell_size_) * size_t(bpp_) / 8;
  auto raw = inflate_zlib(std::span<const uint8_t>(sbm_).subspan(m->sbm_offset), raw_bytes);
  if (!raw || raw->size() != raw_bytes) return false;

  out_w = cell_size_;
  out_h = cell_size_;
  out_coverage.resize(size_t(cell_size_) * size_t(cell_size_));
  if (bpp_ == 4) {
    // Low nibble is even pixel (2x), high nibble is odd pixel (2x+1).
    const size_t stride = size_t(cell_size_) / 2;
    for (int y = 0; y < cell_size_; ++y) {
      for (int x = 0; x < cell_size_; ++x) {
        const uint8_t byte = (*raw)[size_t(y) * stride + size_t(x >> 1)];
        const uint8_t nib = (x & 1) ? uint8_t(byte >> 4) : uint8_t(byte & 0x0F);
        out_coverage[size_t(y) * size_t(cell_size_) + size_t(x)] = uint8_t(nib * 17u);
      }
    }
  } else {
    out_coverage = std::move(*raw);
  }
  return true;
}

bool CjkFont::rasterize_scaled(uint32_t code_point, int target_px, std::vector<uint8_t>& out_coverage, int& out_w,
                               int& out_h, float& out_advance) const {
  const CjkGlyphMetrics* m = metrics(code_point);
  if (!m) return false;
  std::vector<uint8_t> src;
  int sw = 0, sh = 0;
  if (!decode_glyph(code_point, src, sw, sh)) return false;

  target_px = std::clamp(target_px, 6, 128);
  const float scale = float(target_px) / float(std::max(1, sh));
  out_w = std::max(1, int(std::ceil(float(sw) * scale)));
  out_h = target_px;
  out_advance = std::max(2.0f, float(std::max(1, m->advance())) * scale);
  out_coverage.assign(size_t(out_w) * size_t(out_h), 0);

  for (int y = 0; y < out_h; ++y) {
    const float sy = (float(y) + 0.5f) / scale - 0.5f;
    const int y0 = std::clamp(int(std::floor(sy)), 0, sh - 1);
    const int y1 = std::clamp(y0 + 1, 0, sh - 1);
    const float fy = std::clamp(sy - float(y0), 0.0f, 1.0f);
    for (int x = 0; x < out_w; ++x) {
      const float sx = (float(x) + 0.5f) / scale - 0.5f;
      const int x0 = std::clamp(int(std::floor(sx)), 0, sw - 1);
      const int x1 = std::clamp(x0 + 1, 0, sw - 1);
      const float fx = std::clamp(sx - float(x0), 0.0f, 1.0f);
      const float c00 = float(src[size_t(y0) * size_t(sw) + size_t(x0)]);
      const float c10 = float(src[size_t(y0) * size_t(sw) + size_t(x1)]);
      const float c01 = float(src[size_t(y1) * size_t(sw) + size_t(x0)]);
      const float c11 = float(src[size_t(y1) * size_t(sw) + size_t(x1)]);
      const float c0 = c00 + (c10 - c00) * fx;
      const float c1 = c01 + (c11 - c01) * fx;
      out_coverage[size_t(y) * size_t(out_w) + size_t(x)] =
          uint8_t(std::clamp(int(std::lround(c0 + (c1 - c0) * fy)), 0, 255));
    }
  }
  return true;
}

}  // namespace fh1
