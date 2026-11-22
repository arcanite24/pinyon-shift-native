// Xbox 360 XDBF / SPA (spa.bin) achievement & icon database parser (Slice 10).
// Reads base, Title Update v4, Horizon Rally, and 1000 Club spa.bin packages
// from the player's own game/content files: achievement definitions (XACH),
// localized string tables (XSTR), title metadata (XTHD/XSTC), and PNG icons.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fh1 {

// Xbox 360 XLanguage IDs used in XDBF kStringTable sections.
enum class SpaLanguage : uint16_t {
  kEnglish = 1,
  kJapanese = 2,
  kGerman = 3,
  kFrench = 4,
  kSpanish = 5,
  kItalian = 6,
  kKorean = 7,
  kTraditionalChinese = 8,
  kPortuguese = 9,
  kPolish = 11,
  kRussian = 12,
  kSwedish = 13,
  kNorwegian = 14,
  kDutch = 15,
};

uint16_t spa_language_id(std::string_view code);

struct SpaAchievement {
  uint16_t id = 0;
  uint16_t label_id = 0;
  uint16_t description_id = 0;
  uint16_t unachieved_id = 0;
  uint32_t image_id = 0;
  uint16_t gamerscore = 0;
  uint32_t flags = 0;
  std::string label;
  std::string description;
  std::string unachieved_description;

  bool show_unachieved() const { return (flags & 0x8u) != 0; }
};

struct SpaDatabase {
  uint32_t title_id = 0;
  uint16_t default_language = uint16_t(SpaLanguage::kEnglish);
  std::vector<SpaAchievement> achievements;
  std::map<uint32_t, std::vector<uint8_t>> images;  // image_id -> raw PNG bytes

  bool empty() const { return achievements.empty(); }
  const SpaAchievement* find(uint16_t id) const;
  const std::vector<uint8_t>* find_image(uint32_t image_id) const;

  // Merges achievements and icon blobs from `other` (keeping existing entries
  // intact unless `other` fills missing strings/icons, and appending new IDs).
  void merge(const SpaDatabase& other);
};

// Parses an XDBF/SPA binary blob (such as spa.bin).
bool parse_spa_xdbf(std::span<const uint8_t> bytes, SpaDatabase& out, uint16_t language_id = 1,
                    std::string* error = nullptr);
bool load_spa_file(const std::filesystem::path& path, SpaDatabase& out, uint16_t language_id = 1,
                   std::string* error = nullptr);

// Scans `game_root` and `state_root` (including Xbox 360 DLC content folders
// under `00000002/*/spa.bin` and `user/0000000000000000/4D5309C9/00000002/*/spa.bin`)
// and merges all discovered spa.bin packages into `out`.
size_t load_spa_packages(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                         SpaDatabase& out, std::string_view language = "EN", std::string* note = nullptr);

}  // namespace fh1
