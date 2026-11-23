#include "launcher_font.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>

namespace launcher {
namespace {

struct TtfFace {
  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  int ascent = 0;
  int descent = 0;
  int line_gap = 0;
  bool loaded = false;

  bool load_file(const std::filesystem::path& p, int font_index = 0) {
    std::error_code ec;
    if (p.empty() || !std::filesystem::is_regular_file(p, ec)) return false;
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto sz = in.tellg();
    if (sz <= 0 || sz > 64 * 1024 * 1024) return false;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(buf.data()), sz)) return false;
    const int offset = stbtt_GetFontOffsetForIndex(buf.data(), font_index);
    if (offset < 0) return false;
    stbtt_fontinfo fi{};
    if (!stbtt_InitFont(&fi, buf.data(), offset)) return false;
    data = std::move(buf);
    info = fi;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    loaded = true;
    return true;
  }
};

struct CachedGlyph {
  std::array<float, 4> uv{};
  float xoff = 0.0f;
  float yoff = 0.0f;
  float bw = 0.0f;
  float bh = 0.0f;
  float advance = 0.0f;
  bool visible = false;
};

struct FontSystem {
  bool initialized = false;
  TtfFace faces[3];  // 0: Regular, 1: SemiBold, 2: Mono
  std::map<std::tuple<int, uint32_t, int>, CachedGlyph> glyphs;
  uint8_t gamma_lut[256]{};

  FontSystem() {
    // Gentle stem darkening for light-on-dark UI legibility.
    for (int i = 0; i < 256; ++i) {
      const float v = float(i) / 255.0f;
      gamma_lut[i] = uint8_t(std::clamp(std::lround(std::pow(v, 0.86f) * 255.0f), 0l, 255l));
    }
  }

  const TtfFace* resolve_face(FontFace f, int& out_idx) const {
    const int idx = std::clamp(int(f), 0, 2);
    if (faces[idx].loaded) {
      out_idx = idx;
      return &faces[idx];
    }
    if (faces[0].loaded) {
      out_idx = 0;
      return &faces[0];
    }
    return nullptr;
  }

  const CachedGlyph& get_glyph(int face_idx, const TtfFace& face, uint32_t cp, int px_key) {
    const auto key = std::make_tuple(face_idx, cp, px_key);
    if (auto it = glyphs.find(key); it != glyphs.end()) return it->second;

    CachedGlyph g{};
    const float scale = stbtt_ScaleForPixelHeight(&face.info, float(px_key));
    int gi = stbtt_FindGlyphIndex(&face.info, int(cp));
    if (gi == 0) {
      // Graceful ASCII fallbacks for common typographic punctuation.
      uint32_t alt = '?';
      switch (cp) {
        case 0x00B7: alt = '|'; break;   // middle dot
        case 0x2014: alt = '-'; break;   // em dash
        case 0x2013: alt = '-'; break;   // en dash
        case 0x00D7: alt = 'x'; break;   // multiplication sign
        case 0x00B0: alt = '\''; break;  // degree
        case 0x2026: alt = '.'; break;   // ellipsis
        default: break;
      }
      gi = stbtt_FindGlyphIndex(&face.info, int(alt));
    }

    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&face.info, gi, &adv, &lsb);
    g.advance = float(adv) * scale;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&face.info, gi, scale, scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w > 0 && h > 0 && w <= 256 && h <= 256) {
      std::vector<uint8_t> raw(size_t(w) * size_t(h), 0);
      stbtt_MakeGlyphBitmap(&face.info, raw.data(), w, h, w, scale, scale, gi);

      // Add a 1px transparent border around each glyph so bilinear filtering never bleeds.
      const uint32_t pw = uint32_t(w + 2);
      const uint32_t ph = uint32_t(h + 2);
      std::vector<uint8_t> rgba(size_t(pw) * size_t(ph) * 4u, 0);
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const uint8_t a = gamma_lut[raw[size_t(y) * size_t(w) + size_t(x)]];
          const size_t dst = (size_t(y + 1) * size_t(pw) + size_t(x + 1)) * 4u;
          rgba[dst + 0] = 255;
          rgba[dst + 1] = 255;
          rgba[dst + 2] = 255;
          rgba[dst + 3] = a;
        }
      }
      // Keep transparent border RGB white so linear filtering never introduces dark fringes.
      for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = 255;
        rgba[i + 1] = 255;
        rgba[i + 2] = 255;
      }

      char akey[64];
      std::snprintf(akey, sizeof akey, "lfont:%d:%u:%d", face_idx, cp, px_key);
      if (carview::ui::atlas_image(akey, pw, ph, rgba, g.uv)) {
        g.xoff = float(x0 - 1);
        g.yoff = float(y0 - 1);
        g.bw = float(pw);
        g.bh = float(ph);
        g.visible = true;
      }
    }

    auto [it, _] = glyphs.emplace(key, g);
    return it->second;
  }
};

FontSystem& fs() {
  static FontSystem s;
  return s;
}

uint32_t next_utf8(std::string_view s, size_t& i) {
  const uint8_t c0 = uint8_t(s[i++]);
  if (c0 < 0x80) return c0;
  if ((c0 & 0xE0) == 0xC0 && i < s.size()) {
    const uint8_t c1 = uint8_t(s[i++]);
    return ((uint32_t(c0 & 0x1F) << 6) | uint32_t(c1 & 0x3F));
  }
  if ((c0 & 0xF0) == 0xE0 && i + 1 < s.size()) {
    const uint8_t c1 = uint8_t(s[i++]);
    const uint8_t c2 = uint8_t(s[i++]);
    return ((uint32_t(c0 & 0x0F) << 12) | (uint32_t(c1 & 0x3F) << 6) | uint32_t(c2 & 0x3F));
  }
  if ((c0 & 0xF8) == 0xF0 && i + 2 < s.size()) {
    const uint8_t c1 = uint8_t(s[i++]);
    const uint8_t c2 = uint8_t(s[i++]);
    const uint8_t c3 = uint8_t(s[i++]);
    return ((uint32_t(c0 & 0x07) << 18) | (uint32_t(c1 & 0x3F) << 12) | (uint32_t(c2 & 0x3F) << 6) |
            uint32_t(c3 & 0x3F));
  }
  return '?';
}

carview::ui::TextStyle fallback_style(FontFace face, float px_size, uint32_t color, carview::ui::Align align) {
  carview::ui::TextStyle ts;
  ts.size = std::max(8.0f, px_size * 0.72f);
  ts.weight = face == FontFace::kSemiBold ? 0.13f : 0.10f;
  ts.color = color;
  ts.align = align;
  ts.font = face == FontFace::kSemiBold ? carview::ui::Font::kA : carview::ui::Font::kB;
  ts.keep_case = true;
  return ts;
}

bool try_load_candidates(TtfFace& dst, const std::vector<std::filesystem::path>& paths) {
  for (const auto& p : paths) {
    if (dst.load_file(p)) return true;
  }
  return false;
}

}  // namespace

bool init_launcher_fonts() {
  FontSystem& s = fs();
  if (s.initialized) return s.faces[0].loaded;
  s.initialized = true;

  std::vector<std::filesystem::path> reg_paths;
  std::vector<std::filesystem::path> bold_paths;
  std::vector<std::filesystem::path> mono_paths;

  if (const char* custom = std::getenv("SHIFT_LAUNCHER_FONT"); custom && *custom) {
    reg_paths.emplace_back(custom);
  }

#ifdef _WIN32
  std::filesystem::path win_fonts = "C:/Windows/Fonts";
  if (const char* wr = std::getenv("WINDIR"); wr && *wr) {
    win_fonts = std::filesystem::path(wr) / "Fonts";
  }
  reg_paths.push_back(win_fonts / "segoeui.ttf");
  reg_paths.push_back(win_fonts / "arial.ttf");
  reg_paths.push_back(win_fonts / "calibri.ttf");
  reg_paths.push_back(win_fonts / "tahoma.ttf");

  bold_paths.push_back(win_fonts / "seguisb.ttf");
  bold_paths.push_back(win_fonts / "segoeuib.ttf");
  bold_paths.push_back(win_fonts / "arialbd.ttf");
  bold_paths.push_back(win_fonts / "calibrib.ttf");

  mono_paths.push_back(win_fonts / "consola.ttf");
  mono_paths.push_back(win_fonts / "lucon.ttf");
  mono_paths.push_back(win_fonts / "cour.ttf");
#elif defined(__APPLE__)
  reg_paths = {
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
      "/System/Library/Fonts/HelveticaNeue.ttc",
  };
  bold_paths = {
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
  };
  mono_paths = {
      "/System/Library/Fonts/SFNSMono.ttf",
      "/System/Library/Fonts/Menlo.ttc",
      "/System/Library/Fonts/Supplemental/Courier New.ttf",
  };
#else
  reg_paths = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
      "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
  };
  bold_paths = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
  };
  mono_paths = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
  };
#endif

  try_load_candidates(s.faces[0], reg_paths);
  try_load_candidates(s.faces[1], bold_paths);
  try_load_candidates(s.faces[2], mono_paths);
  return s.faces[0].loaded;
}

bool launcher_fonts_ready() {
  init_launcher_fonts();
  return fs().faces[0].loaded;
}

float measure_text(FontFace face, float px_size, std::string_view text) {
  if (text.empty()) return 0.0f;
  init_launcher_fonts();
  int fidx = 0;
  const TtfFace* tf = fs().resolve_face(face, fidx);
  if (!tf) {
    return carview::ui::List::measure(text, fallback_style(face, px_size, 0xFFFFFFFFu, carview::ui::Align::kLeft));
  }
  const int px_key = std::max(8, int(std::lround(px_size)));
  const float scale = stbtt_ScaleForPixelHeight(&tf->info, float(px_key));
  float pen = 0.0f;
  uint32_t prev_cp = 0;
  size_t i = 0;
  while (i < text.size()) {
    const uint32_t cp = next_utf8(text, i);
    if (prev_cp != 0) pen += float(stbtt_GetCodepointKernAdvance(&tf->info, int(prev_cp), int(cp))) * scale;
    pen += fs().get_glyph(fidx, *tf, cp, px_key).advance;
    prev_cp = cp;
  }
  return pen;
}

std::string ellipsize_text(FontFace face, float px_size, std::string_view text, float max_width) {
  if (max_width <= 0.0f || text.empty()) return std::string(text);
  if (measure_text(face, px_size, text) <= max_width) return std::string(text);

  const std::string_view dots = "...";
  const float dots_w = measure_text(face, px_size, dots);
  if (dots_w >= max_width) return std::string(dots);

  // Collect UTF-8 character boundary offsets.
  std::vector<size_t> boundaries;
  boundaries.push_back(0);
  size_t i = 0;
  while (i < text.size()) {
    next_utf8(text, i);
    boundaries.push_back(i);
  }

  size_t lo = 0;
  size_t hi = boundaries.size() - 1;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo + 1) / 2;
    const float w = measure_text(face, px_size, text.substr(0, boundaries[mid]));
    if (w + dots_w <= max_width) lo = mid;
    else hi = mid - 1;
  }
  std::string out(text.substr(0, boundaries[lo]));
  while (!out.empty() && out.back() == ' ') out.pop_back();
  out += dots;
  return out;
}

float draw_text(carview::ui::List& list, float x, float y, std::string_view text, FontFace face,
                float px_size, uint32_t color, carview::ui::Align align, float max_width) {
  if (text.empty()) return 0.0f;
  init_launcher_fonts();

  std::string owned;
  std::string_view s = text;
  if (max_width > 0.0f) {
    owned = ellipsize_text(face, px_size, text, max_width);
    s = owned;
  }

  int fidx = 0;
  const TtfFace* tf = fs().resolve_face(face, fidx);
  if (!tf) {
    auto ts = fallback_style(face, px_size, color, align);
    return list.text(x, y + (px_size - ts.size) * 0.5f, s, ts);
  }

  const int px_key = std::max(8, int(std::lround(px_size)));
  const float scale = stbtt_ScaleForPixelHeight(&tf->info, float(px_key));
  const float total_w = measure_text(face, float(px_key), s);

  float pen_x = x;
  if (align == carview::ui::Align::kCenter) pen_x = x - 0.5f * total_w;
  else if (align == carview::ui::Align::kRight) pen_x = x - total_w;
  pen_x = std::floor(pen_x + 0.5f);

  const float baseline = std::floor(y + float(tf->ascent) * scale + 0.5f);
  uint32_t prev_cp = 0;
  size_t i = 0;
  while (i < s.size()) {
    const uint32_t cp = next_utf8(s, i);
    if (prev_cp != 0) {
      pen_x += float(stbtt_GetCodepointKernAdvance(&tf->info, int(prev_cp), int(cp))) * scale;
    }
    const CachedGlyph& g = fs().get_glyph(fidx, *tf, cp, px_key);
    if (g.visible) {
      const float gx0 = std::floor(pen_x + g.xoff + 0.5f);
      const float gy0 = baseline + g.yoff;
      list.image(gx0, gy0, gx0 + g.bw, gy0 + g.bh, g.uv, color);
    }
    pen_x += g.advance;
    prev_cp = cp;
  }
  return total_w;
}

float draw_wrapped_text(carview::ui::List& list, float x, float y, float max_width, float line_height,
                        int max_lines, std::string_view text, FontFace face, float px_size, uint32_t color) {
  if (text.empty() || max_lines <= 0 || max_width <= 8.0f) return 0.0f;

  // Split by whitespace into words, preserving explicit newlines.
  std::vector<std::string> lines;
  std::string current;
  size_t pos = 0;
  while (pos < text.size()) {
    if (text[pos] == '\n') {
      lines.push_back(current);
      current.clear();
      ++pos;
      continue;
    }
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;
    size_t end = pos;
    while (end < text.size() && text[end] != ' ' && text[end] != '\n') ++end;
    const std::string_view word = text.substr(pos, end - pos);
    pos = end;

    std::string candidate = current.empty() ? std::string(word) : (current + " " + std::string(word));
    if (measure_text(face, px_size, candidate) <= max_width || current.empty()) {
      current = std::move(candidate);
    } else {
      lines.push_back(std::move(current));
      current = std::string(word);
    }
  }
  if (!current.empty()) lines.push_back(std::move(current));

  const int n = std::min<int>(int(lines.size()), max_lines);
  for (int i = 0; i < n; ++i) {
    std::string line = lines[size_t(i)];
    if (i == n - 1 && int(lines.size()) > max_lines) {
      // Append remaining text so ellipsize_text places "..." cleanly at the edge.
      line += " " + lines[size_t(i + 1)];
    }
    draw_text(list, x, y + float(i) * line_height, line, face, px_size, color, carview::ui::Align::kLeft, max_width);
  }
  return float(n) * line_height;
}

}  // namespace launcher
