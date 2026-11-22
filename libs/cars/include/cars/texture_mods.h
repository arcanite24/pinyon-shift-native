// Replacement textures (ours, Slice 13): loose PNG, TGA or DDS files in a
// mod's textures/ folder that take the place of a game texture when it is
// uploaded, up to 8 times its size, reloadable while the game runs (F5 or
// the Mods page's RELOAD TEXTURES).
//
// A file is found by the texture's content hash or its name:
//   textures/<16 hex digits>.<ext>  XXH64 (seed 0) of the texture's base
//                                   level as decoded from the game file
//                                   (texels or blocks, rows top first)
//   textures/<name>.<ext>           the texture's lower-case member name
//                                   without its extension, '/' separated,
//                                   ':' written '_' (a car's
//                                   "nodamage_lod0", a world texture's
//                                   "colorado__0x<id>")
// The first mod in the load order with a file wins. A replacement must keep
// the texture's aspect and be 1, 2, 4 or 8 times its base size; others are
// refused (and listed). DDS files may be uncompressed 32-bit (RGBA or BGRA)
// or BC1/BC2/BC3; every replacement is uploaded as RGBA8 with a box-filtered
// mip chain.
//
// Live reload: reload() rescans the folders, compares each file's size,
// time and content hash with the last scan and returns the keys that
// changed (added, edited or removed), bumping generation(). A cache keyed
// by texture (TextureCache) drops what reload() names, so the next upload
// reads the new file; the renderer re-uploads the textures it holds.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cars/bake_format.h"

namespace cars::texture_mods {

uint64_t xxh64(const void* data, size_t len, uint64_t seed = 0);
uint64_t content_hash(std::span<const uint8_t> base_level);
std::string hex16(uint64_t h);
// "c:<16 hex>" or "n:<name>" (lower case, '/', no extension).
std::string content_key(uint64_t hash);
std::string name_key(std::string_view name);

struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // straight alpha, rows top first
};

// PNG/TGA/JPG (with stb_image) or DDS. False with `error` otherwise.
bool decode_image(const std::filesystem::path& file, Image& out, std::string* error);
bool decode_dds(std::span<const uint8_t> bytes, Image& out, std::string* error);

// 1, 2, 4 or 8 when (w, h) is that multiple of (base_w, base_h); else 0.
int scale_factor(uint32_t base_w, uint32_t base_h, uint32_t w, uint32_t h);

// An RGBA8 texture with its box-filtered mip chain down to 1x1.
cars::bake::Texture to_texture(const Image& img, const std::string& name);

class Registry {
 public:
  // Scans the mods' textures/ folders (the winner first).
  void scan(const std::vector<std::filesystem::path>& mod_dirs_winner_first);
  void clear();
  size_t size() const;
  uint32_t generation() const;
  // The replacement file for a texture, by content hash then name.
  std::optional<std::filesystem::path> find(uint64_t content, std::string_view name) const;
  // Rescans the same folders; returns the keys whose file changed.
  std::vector<std::string> reload();
  // The keys a texture answers to.
  static std::vector<std::string> keys_for(uint64_t content, std::string_view name);
  // Replacements refused at their last load (key: why).
  std::map<std::string, std::string> refused() const;
  void refuse(const std::string& key, const std::string& why);

 private:
  struct File {
    std::filesystem::path path;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t hash = 0;
  };
  std::map<std::string, File> scan_files() const;
  mutable std::mutex mutex_;
  std::vector<std::filesystem::path> dirs_;
  std::map<std::string, File> files_;
  std::map<std::string, std::string> refused_;
  uint32_t generation_ = 0;
};

Registry& registry();

// A decoded-replacement cache keyed by texture key; reload() results drop
// entries (live reload cache invalidation).
class TextureCache {
 public:
  // The decoded replacement for `key` loaded from `file`, decoding on a miss.
  const Image* get(const std::string& key, const std::filesystem::path& file, std::string* error);
  // Drops the named keys; returns how many were cached.
  size_t invalidate(const std::vector<std::string>& keys);
  size_t size() const { return entries_.size(); }
  size_t decodes() const { return decodes_; }

 private:
  std::map<std::string, Image> entries_;
  size_t decodes_ = 0;
};

// The upload hook: when the registry has a valid replacement for `in`
// (its base level's hash or its name), fills `out` with it and returns
// true. `why` receives a refusal (wrong scale, unreadable file).
bool substitute(const cars::bake::Texture& in, cars::bake::Texture& out, std::string* why = nullptr);
// Whether any replacement is registered (a cheap check before hashing).
bool any();

}  // namespace cars::texture_mods
