#include "cars/texture_mods.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(SHIFT_HAVE_STB)
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4505 4244 4100 4456 4457 4701 4702)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace cars::texture_mods {
namespace fs = std::filesystem;
namespace {

constexpr uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                   P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;

uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
uint64_t read64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;  // little-endian hosts (x86-64, ARM64)
}
uint32_t read32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t round64(uint64_t acc, uint64_t input) {
  acc += input * P2;
  acc = rotl(acc, 31);
  return acc * P1;
}
uint64_t merge_round(uint64_t acc, uint64_t val) {
  acc ^= round64(0, val);
  return acc * P1 + P4;
}

std::string lower_ascii(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool read_bytes(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

bool is_hex16(const std::string& s) {
  return s.size() == 16 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

bool image_ext(const std::string& ext) {
  return ext == ".png" || ext == ".dds" || ext == ".tga" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
}

// ---- BC1/BC2/BC3 ----

void rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

// A BC1 colour block into 16 RGBA texels; `four` forces the 4-colour mode.
void bc1_block(const uint8_t* b, uint8_t px[16][4], bool four) {
  const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  uint8_t pal[4][4] = {};
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (four || c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  const uint32_t idx = read32(b + 4);
  for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

bool decode_bc(const uint8_t* data, size_t size, uint32_t w, uint32_t h, int bc, Image& out) {
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t block = bc == 1 ? 8 : 16;
  if (size < size_t(bw) * bh * block) return false;
  out.width = w;
  out.height = h;
  out.rgba.assign(size_t(w) * h * 4, 0);
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* b = data + (size_t(by) * bw + bx) * block;
      uint8_t px[16][4];
      if (bc == 1) {
        bc1_block(b, px, false);
      } else {
        bc1_block(b + 8, px, true);
        if (bc == 2) {
          for (int i = 0; i < 16; ++i) {
            const uint8_t a = uint8_t((b[i / 2] >> ((i & 1) * 4)) & 15);
            px[i][3] = uint8_t(a * 17);
          }
        } else {
          const uint8_t a0 = b[0], a1 = b[1];
          uint8_t pal[8];
          pal[0] = a0, pal[1] = a1;
          if (a0 > a1) {
            for (int k = 1; k < 7; ++k) pal[k + 1] = uint8_t(((7 - k) * a0 + k * a1) / 7);
          } else {
            for (int k = 1; k < 5; ++k) pal[k + 1] = uint8_t(((5 - k) * a0 + k * a1) / 5);
            pal[6] = 0, pal[7] = 255;
          }
          uint64_t bits = 0;
          for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
          for (int i = 0; i < 16; ++i) px[i][3] = pal[(bits >> (3 * i)) & 7];
        }
      }
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + uint32_t(i % 4), y = by * 4 + uint32_t(i / 4);
        if (x < w && y < h) std::memcpy(&out.rgba[(size_t(y) * w + x) * 4], px[i], 4);
      }
    }
  return true;
}

Registry g_registry;
std::mutex g_cache_mutex;
TextureCache g_cache;

}  // namespace

uint64_t xxh64(const void* data, size_t len, uint64_t seed) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  const uint8_t* end = p + len;
  uint64_t h;
  if (len >= 32) {
    uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
    const uint8_t* limit = end - 32;
    do {
      v1 = round64(v1, read64(p));
      v2 = round64(v2, read64(p + 8));
      v3 = round64(v3, read64(p + 16));
      v4 = round64(v4, read64(p + 24));
      p += 32;
    } while (p <= limit);
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h = merge_round(h, v1);
    h = merge_round(h, v2);
    h = merge_round(h, v3);
    h = merge_round(h, v4);
  } else {
    h = seed + P5;
  }
  h += uint64_t(len);
  while (p + 8 <= end) {
    h ^= round64(0, read64(p));
    h = rotl(h, 27) * P1 + P4;
    p += 8;
  }
  if (p + 4 <= end) {
    h ^= uint64_t(read32(p)) * P1;
    h = rotl(h, 23) * P2 + P3;
    p += 4;
  }
  while (p < end) {
    h ^= uint64_t(*p) * P5;
    h = rotl(h, 11) * P1;
    ++p;
  }
  h ^= h >> 33;
  h *= P2;
  h ^= h >> 29;
  h *= P3;
  h ^= h >> 32;
  return h;
}

uint64_t content_hash(std::span<const uint8_t> base_level) { return xxh64(base_level.data(), base_level.size(), 0); }

std::string hex16(uint64_t h) {
  static const char* d = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i, h >>= 4) s[size_t(i)] = d[h & 15];
  return s;
}

std::string content_key(uint64_t hash) { return "c:" + hex16(hash); }

std::string name_key(std::string_view name) {
  std::string n = lower_ascii(std::string(name));
  std::replace(n.begin(), n.end(), '\\', '/');
  std::replace(n.begin(), n.end(), ':', '_');  // "colorado:_0x..." is not a file name on Windows
  const auto slash = n.rfind('/');
  const auto dot = n.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) n.resize(dot);
  return "n:" + n;
}

bool decode_dds(std::span<const uint8_t> b, Image& out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (b.size() < 128 || std::memcmp(b.data(), "DDS ", 4) != 0) return fail("not a DDS file");
  const uint32_t h = read32(&b[12]), w = read32(&b[16]);
  const uint32_t pf_flags = read32(&b[80]);
  const uint32_t fourcc = read32(&b[84]);
  const uint32_t bits = read32(&b[88]);
  const uint32_t rmask = read32(&b[92]), gmask = read32(&b[96]), bmask = read32(&b[100]), amask = read32(&b[104]);
  size_t offset = 128;
  if (w == 0 || h == 0 || w > 16384 || h > 16384) return fail("bad DDS size");
  auto cc = [](const char* s) { return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24; };
  int bc = 0;
  bool bgra = false, rgba = false;
  if (pf_flags & 0x4) {  // DDPF_FOURCC
    if (fourcc == cc("DXT1")) bc = 1;
    else if (fourcc == cc("DXT2") || fourcc == cc("DXT3")) bc = 2;
    else if (fourcc == cc("DXT4") || fourcc == cc("DXT5")) bc = 3;
    else if (fourcc == cc("DX10")) {
      if (b.size() < 148) return fail("truncated DX10 header");
      const uint32_t dxgi = read32(&b[128]);
      offset = 148;
      if (dxgi == 70 || dxgi == 71 || dxgi == 72) bc = 1;
      else if (dxgi == 73 || dxgi == 74 || dxgi == 75) bc = 2;
      else if (dxgi == 76 || dxgi == 77 || dxgi == 78) bc = 3;
      else if (dxgi == 28 || dxgi == 29) rgba = true;
      else if (dxgi == 87 || dxgi == 91) bgra = true;
      else return fail("unsupported DXGI format");
    } else {
      return fail("unsupported DDS FourCC");
    }
  } else if (bits == 32) {
    if (rmask == 0x000000FFu && gmask == 0x0000FF00u && bmask == 0x00FF0000u) rgba = true;
    else if (rmask == 0x00FF0000u && gmask == 0x0000FF00u && bmask == 0x000000FFu) bgra = true;
    else return fail("unsupported 32-bit DDS layout");
    (void)amask;
  } else {
    return fail("unsupported DDS pixel format");
  }
  const uint8_t* data = b.data() + offset;
  const size_t size = b.size() - offset;
  if (bc) {
    if (!decode_bc(data, size, w, h, bc, out)) return fail("truncated DDS blocks");
    return true;
  }
  if (size < size_t(w) * h * 4) return fail("truncated DDS texels");
  out.width = w;
  out.height = h;
  out.rgba.assign(data, data + size_t(w) * h * 4);
  if (bgra)
    for (size_t i = 0; i < out.rgba.size(); i += 4) std::swap(out.rgba[i], out.rgba[i + 2]);
  if (!(pf_flags & 0x1) && !rgba)  // no alpha channel: opaque
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
  return true;
}

bool decode_image(const fs::path& file, Image& out, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!read_bytes(file, bytes)) {
    if (error) *error = "cannot read " + file.string();
    return false;
  }
  if (lower_ascii(file.extension().string()) == ".dds") return decode_dds(bytes, out, error);
#if defined(SHIFT_HAVE_STB)
  int w = 0, h = 0, comp = 0;
  stbi_uc* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
  if (!px) {
    if (error) *error = std::string("cannot decode: ") + (stbi_failure_reason() ? stbi_failure_reason() : "?");
    return false;
  }
  out.width = uint32_t(w);
  out.height = uint32_t(h);
  out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
  stbi_image_free(px);
  return true;
#else
  if (error) *error = "this build reads only DDS replacements";
  return false;
#endif
}

int scale_factor(uint32_t base_w, uint32_t base_h, uint32_t w, uint32_t h) {
  if (base_w == 0 || base_h == 0) return 0;
  for (int k : {1, 2, 4, 8})
    if (w == base_w * uint32_t(k) && h == base_h * uint32_t(k)) return k;
  return 0;
}

cars::bake::Texture to_texture(const Image& img, const std::string& name) {
  cars::bake::Texture t;
  t.name = name;
  t.format = cars::bake::TextureFormat::kRgba8;
  t.levels.push_back({img.width, img.height, img.rgba});
  while (t.levels.back().width > 1 || t.levels.back().height > 1) {
    const auto& src = t.levels.back();
    const uint32_t w = std::max(1u, src.width / 2), h = std::max(1u, src.height / 2);
    cars::bake::TextureLevel dst{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
        for (int c = 0; c < 4; ++c) {
          uint32_t sum = 0, n = 0;
          for (uint32_t dy = 0; dy < 2; ++dy)
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint32_t sx = std::min(src.width - 1, x * 2 + dx), sy = std::min(src.height - 1, y * 2 + dy);
              sum += src.data[(size_t(sy) * src.width + sx) * 4 + size_t(c)];
              ++n;
            }
          dst.data[(size_t(y) * w + x) * 4 + size_t(c)] = uint8_t((sum + n / 2) / n);
        }
    t.levels.push_back(std::move(dst));
  }
  return t;
}

// ---- Registry ----

std::map<std::string, Registry::File> Registry::scan_files() const {
  std::map<std::string, File> out;
  std::error_code ec;
  for (const auto& dir : dirs_) {
    const fs::path root = dir / "textures";
    if (!fs::is_directory(root, ec)) continue;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
      if (it->is_regular_file(ec) && image_ext(lower_ascii(it->path().extension().string())))
        files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      std::string rel = lower_ascii(fs::relative(f, root, ec).generic_string());
      rel.resize(rel.size() - f.extension().string().size());
      const std::string key = is_hex16(rel) ? "c:" + rel : "n:" + rel;
      if (out.count(key)) continue;  // an earlier (winning) mod has it
      File rec;
      rec.path = f;
      rec.size = uint64_t(fs::file_size(f, ec));
      rec.mtime = int64_t(fs::last_write_time(f, ec).time_since_epoch().count());
      std::vector<uint8_t> bytes;
      if (read_bytes(f, bytes)) rec.hash = xxh64(bytes.data(), bytes.size());
      out.emplace(key, std::move(rec));
    }
  }
  return out;
}

void Registry::scan(const std::vector<fs::path>& mod_dirs_winner_first) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dirs_ = mod_dirs_winner_first;
    files_ = scan_files();
    refused_.clear();
    ++generation_;
  }
  std::lock_guard<std::mutex> cl(g_cache_mutex);
  g_cache = TextureCache{};
}

void Registry::clear() { scan({}); }

size_t Registry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return files_.size();
}

uint32_t Registry::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

std::vector<std::string> Registry::keys_for(uint64_t content, std::string_view name) {
  std::vector<std::string> out{content_key(content)};
  if (!name.empty()) out.push_back(name_key(name));
  return out;
}

std::optional<fs::path> Registry::find(uint64_t content, std::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& key : keys_for(content, name))
    if (auto it = files_.find(key); it != files_.end()) return it->second.path;
  return std::nullopt;
}

std::vector<std::string> Registry::reload() {
  std::vector<std::string> changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = scan_files();
    for (const auto& [key, f] : next) {
      auto it = files_.find(key);
      if (it == files_.end() || it->second.path != f.path || it->second.size != f.size ||
          it->second.mtime != f.mtime || it->second.hash != f.hash)
        changed.push_back(key);
    }
    for (const auto& [key, f] : files_)
      if (!next.count(key)) changed.push_back(key);
    files_ = std::move(next);
    if (!changed.empty()) ++generation_;
    for (const auto& k : changed) refused_.erase(k);
  }
  std::lock_guard<std::mutex> cl(g_cache_mutex);
  g_cache.invalidate(changed);
  return changed;
}

std::map<std::string, std::string> Registry::refused() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return refused_;
}

void Registry::refuse(const std::string& key, const std::string& why) {
  std::lock_guard<std::mutex> lock(mutex_);
  refused_[key] = why;
}

Registry& registry() { return g_registry; }

// ---- TextureCache ----

const Image* TextureCache::get(const std::string& key, const fs::path& file, std::string* error) {
  if (auto it = entries_.find(key); it != entries_.end()) return &it->second;
  Image img;
  if (!decode_image(file, img, error)) return nullptr;
  ++decodes_;
  return &(entries_[key] = std::move(img));
}

size_t TextureCache::invalidate(const std::vector<std::string>& keys) {
  size_t n = 0;
  for (const auto& k : keys) n += entries_.erase(k);
  return n;
}

bool any() { return g_registry.size() > 0; }

bool substitute(const cars::bake::Texture& in, cars::bake::Texture& out, std::string* why) {
  if (in.levels.empty() || !any()) return false;
  const auto& base = in.levels[0];
  const uint64_t h = content_hash(base.data);
  std::string key;
  fs::path file;
  if (auto f = g_registry.find(h, {})) {
    key = content_key(h), file = *f;
  } else if (auto g = in.name.empty() ? std::nullopt : g_registry.find(~h, in.name)) {
    key = name_key(in.name), file = *g;
  }
  if (key.empty()) return false;
  std::lock_guard<std::mutex> cl(g_cache_mutex);
  std::string err;
  const Image* img = g_cache.get(key, file, &err);
  if (!img) {
    g_registry.refuse(key, err);
    if (why) *why = file.filename().string() + ": " + err;
    return false;
  }
  if (!scale_factor(base.width, base.height, img->width, img->height)) {
    const std::string r = std::to_string(img->width) + "x" + std::to_string(img->height) + " is not 1, 2, 4 or 8 times " +
                          std::to_string(base.width) + "x" + std::to_string(base.height);
    g_registry.refuse(key, r);
    if (why) *why = file.filename().string() + ": " + r;
    return false;
  }
  out = to_texture(*img, in.name);
  return true;
}

}  // namespace cars::texture_mods
