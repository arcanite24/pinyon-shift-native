#include "reference.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>

namespace carview {

namespace {

// A small JSON reader: objects, arrays, strings, numbers, true/false/null.
struct Json {
  enum Type { kNull, kBool, kNumber, kString, kArray, kObject } type = kNull;
  double number = 0;
  std::string string;
  std::vector<Json> array;
  std::map<std::string, Json> object;
};

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}
  bool parse(Json& out, std::string& error) {
    if (!value(out)) {
      error = "JSON error at byte " + std::to_string(pos_);
      return false;
    }
    space();
    if (pos_ != s_.size()) {
      error = "JSON: trailing bytes at " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  void space() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
      ++pos_;
  }
  bool literal(const char* word) {
    const size_t n = std::char_traits<char>::length(word);
    if (s_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }
  bool string(std::string& out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      char c = s_[pos_++];
      if (c == '\\') {
        if (pos_ >= s_.size()) return false;
        const char e = s_[pos_++];
        switch (e) {
          case 'n': c = '\n'; break;
          case 't': c = '\t'; break;
          case 'r': c = '\r'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'u':
            if (pos_ + 4 > s_.size()) return false;
            pos_ += 4;  // not needed by the schema: kept as '?'
            c = '?';
            break;
          default: c = e; break;
        }
      }
      out.push_back(c);
    }
    if (pos_ >= s_.size()) return false;
    ++pos_;
    return true;
  }
  bool value(Json& out) {
    space();
    if (pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      out.type = Json::kObject;
      space();
      if (pos_ < s_.size() && s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      for (;;) {
        space();
        std::string key;
        if (!string(key)) return false;
        space();
        if (pos_ >= s_.size() || s_[pos_] != ':') return false;
        ++pos_;
        if (!value(out.object[key])) return false;
        space();
        if (pos_ < s_.size() && s_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < s_.size() && s_[pos_] == '}') {
          ++pos_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++pos_;
      out.type = Json::kArray;
      space();
      if (pos_ < s_.size() && s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      for (;;) {
        out.array.emplace_back();
        if (!value(out.array.back())) return false;
        space();
        if (pos_ < s_.size() && s_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < s_.size() && s_[pos_] == ']') {
          ++pos_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      out.type = Json::kString;
      return string(out.string);
    }
    if (literal("true")) {
      out.type = Json::kBool;
      out.number = 1;
      return true;
    }
    if (literal("false")) {
      out.type = Json::kBool;
      return true;
    }
    if (literal("null")) return true;
    const size_t start = pos_;
    while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-' ||
                                s_[pos_] == '+' || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E'))
      ++pos_;
    if (pos_ == start) return false;
    std::istringstream in(s_.substr(start, pos_ - start));
    in.imbue(std::locale::classic());
    in >> out.number;
    out.type = Json::kNumber;
    return !in.fail();
  }

  const std::string& s_;
  size_t pos_ = 0;
};

}  // namespace

bool read_reference(const std::filesystem::path& path, Reference& out, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot read " + path.string();
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  Json root;
  if (!Parser(text).parse(root, error)) return false;
  if (root.type != Json::kObject) {
    error = path.string() + ": not a JSON object";
    return false;
  }
  auto get = [&](const char* key) -> const Json* {
    auto it = root.object.find(key);
    return it == root.object.end() ? nullptr : &it->second;
  };
  const Json* schema = get("schema");
  out.schema = schema && schema->type == Json::kString ? schema->string : "";
  if (out.schema == "shift-engine.m1-reference.v1") {
    out.version = 1;
  } else if (out.schema == "shift-engine.m1-reference.v2") {
    out.version = 2;
  } else {
    error = path.string() + ": schema \"" + out.schema + "\" is not shift-engine.m1-reference.v1 or v2";
    return false;
  }
  auto text_field = [&](const char* key) -> std::string {
    const Json* j = get(key);
    return j && j->type == Json::kString ? j->string : std::string();
  };
  out.media_name = text_field("media_name");
  if (const Json* id = get("car_id"); id && id->type == Json::kNumber) out.car_id = int64_t(id->number);
  const Json* width = get("width");
  const Json* height = get("height");
  const Json* matrix = get("clip_from_car");
  if (!width || width->type != Json::kNumber || !height || height->type != Json::kNumber ||
      width->number < 1 || height->number < 1 || width->number > 16384 || height->number > 16384) {
    error = path.string() + ": width and height must be numbers from 1 to 16384";
    return false;
  }
  out.width = uint32_t(width->number);
  out.height = uint32_t(height->number);
  if (!matrix || matrix->type != Json::kArray || matrix->array.size() != 16) {
    error = path.string() + ": clip_from_car must be an array of 16 numbers";
    return false;
  }
  for (size_t i = 0; i < 16; ++i) {
    if (matrix->array[i].type != Json::kNumber) {
      error = path.string() + ": clip_from_car must be an array of 16 numbers";
      return false;
    }
    out.clip_from_car[i] = float(matrix->array[i].number);
  }
  // The transparent pass's matrix (checks.transparent_clip_from_car); the
  // main one when absent.
  std::copy(std::begin(out.clip_from_car), std::end(out.clip_from_car), std::begin(out.transparent_clip_from_car));
  out.has_transparent = false;
  if (const Json* checks = get("checks"); checks && checks->type == Json::kObject) {
    if (auto g = checks->object.find("glow_px"); g != checks->object.end() && g->second.type == Json::kNumber)
      out.glow_px = int64_t(g->second.number);
    auto it = checks->object.find("transparent_clip_from_car");
    if (it != checks->object.end() && it->second.type == Json::kArray && it->second.array.size() == 16) {
      bool numbers = true;
      for (const auto& v : it->second.array) numbers &= v.type == Json::kNumber;
      if (numbers) {
        for (size_t i = 0; i < 16; ++i) out.transparent_clip_from_car[i] = float(it->second.array[i].number);
        out.has_transparent = true;
      }
    }
  }
  const std::filesystem::path folder = path.parent_path();
  if (const std::string image = text_field("image"); !image.empty()) out.image = folder / image;
  if (const std::string mask = text_field("mask"); !mask.empty()) out.mask = folder / mask;
  if (const std::string car_only = text_field("image_car_only"); !car_only.empty())
    out.image_car_only = folder / car_only;
  if (const Json* paint = get("paint_rgb");
      paint && paint->type == Json::kArray && paint->array.size() == 3) {
    out.has_paint = true;
    for (size_t i = 0; i < 3; ++i) out.paint_rgb[i] = int(paint->array[i].number);
  }
  return true;
}

bool clip_eye(const float m[16], float eye[3]) {
  double d[16];
  for (int i = 0; i < 16; ++i) d[i] = m[i];
  // The camera centre is the null vector of the x, y and w rows: every
  // point on a ray through it differs only in z.
  const double rows[3][4] = {{d[0], d[4], d[8], d[12]}, {d[1], d[5], d[9], d[13]}, {d[3], d[7], d[11], d[15]}};
  // Cross product generalised to 4D: the vector orthogonal to three rows.
  double n[4];
  for (int i = 0; i < 4; ++i) {
    double minor[3][3];
    for (int r = 0; r < 3; ++r) {
      int k = 0;
      for (int c = 0; c < 4; ++c)
        if (c != i) minor[r][k++] = rows[r][c];
    }
    const double det = minor[0][0] * (minor[1][1] * minor[2][2] - minor[1][2] * minor[2][1]) -
                       minor[0][1] * (minor[1][0] * minor[2][2] - minor[1][2] * minor[2][0]) +
                       minor[0][2] * (minor[1][0] * minor[2][1] - minor[1][1] * minor[2][0]);
    n[i] = (i % 2 ? -1 : 1) * det;
  }
  if (std::fabs(n[3]) < 1e-12) return false;
  for (int k = 0; k < 3; ++k) eye[k] = float(n[k] / n[3]);
  return true;
}

Metric compare(const std::vector<uint8_t>& reference_rgba, const std::vector<uint8_t>& reference_mask,
               const std::vector<uint8_t>& engine_rgba, const std::vector<uint8_t>& engine_mask, uint32_t width,
               uint32_t height) {
  Metric m;
  const size_t n = size_t(width) * height;
  uint64_t both = 0, either = 0;
  double squared = 0;
  for (size_t i = 0; i < n; ++i) {
    const bool r = reference_mask[i] != 0, e = engine_mask[i] != 0;
    both += r && e;
    either += r || e;
    m.reference_pixels += r;
    m.engine_pixels += e;
    if (r) {
      for (int c = 0; c < 3; ++c) {
        const double d = double(reference_rgba[i * 4 + size_t(c)]) - double(engine_rgba[i * 4 + size_t(c)]);
        squared += d * d;
      }
    }
  }
  m.iou = either ? double(both) / double(either) : 1.0;
  const double mse = m.reference_pixels ? squared / (3.0 * double(m.reference_pixels)) : 0;
  m.masked_psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : std::numeric_limits<double>::infinity();
  // Edges: mask pixels with a 4-neighbour outside the mask.
  auto edge = [&](const std::vector<uint8_t>& mask, uint32_t x, uint32_t y) {
    if (!mask[size_t(y) * width + x]) return false;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
      const int nx = int(x) + dx[k], ny = int(y) + dy[k];
      if (nx < 0 || ny < 0 || nx >= int(width) || ny >= int(height) || !mask[size_t(ny) * width + size_t(nx)])
        return true;
    }
    return false;
  };
  uint64_t edges = 0, matched = 0;
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      if (!edge(reference_mask, x, y)) continue;
      ++edges;
      bool hit = false;
      for (int oy = -1; oy <= 1 && !hit; ++oy)
        for (int ox = -1; ox <= 1 && !hit; ++ox) {
          const int nx = int(x) + ox, ny = int(y) + oy;
          if (nx >= 0 && ny >= 0 && nx < int(width) && ny < int(height))
            hit = edge(engine_mask, uint32_t(nx), uint32_t(ny));
        }
      matched += hit;
    }
  m.edge_overlap = edges ? double(matched) / double(edges) : 1.0;
  return m;
}

}  // namespace carview
