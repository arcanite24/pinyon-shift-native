#include "fh1data/spa.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

constexpr uint32_t kXdbfMagic = 0x58444246u;  // "XDBF"
constexpr uint32_t kXachMagic = 0x58414348u;  // "XACH"
constexpr uint32_t kXstrMagic = 0x58535452u;  // "XSTR"
constexpr uint32_t kXstcMagic = 0x58535443u;  // "XSTC"
constexpr uint32_t kXthdMagic = 0x58544844u;  // "XTHD"

constexpr uint16_t kSectionMetadata = 1;
constexpr uint16_t kSectionImage = 2;
constexpr uint16_t kSectionStringTable = 3;

uint16_t u16_be(std::span<const uint8_t> b, size_t off) { return load_be16(b.data() + off); }
uint32_t u32_be(std::span<const uint8_t> b, size_t off) { return load_be32(b.data() + off); }

uint64_t read_u64_be(std::span<const uint8_t> b, size_t off) {
  return (uint64_t(u32_be(b, off)) << 32) | uint64_t(u32_be(b, off + 4));
}

// Cleans occasional Windows-1252 punctuation bytes (such as 0x85 ellipsis in
// FH1 achievement descriptions) into valid UTF-8 so vector fonts render cleanly.
std::string clean_spa_text(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    size_t len = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xE ? 3 : (byte >> 3) == 0x1E ? 4 : 0;
    bool valid = len != 0 && i + len <= text.size();
    for (size_t k = 1; valid && k < len; ++k) {
      valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
    }
    if (valid) {
      out.append(text.substr(i, len));
      i += len;
      continue;
    }
    switch (byte) {
      case 0x85: out += "..."; break;
      case 0x91:
      case 0x92: out += '\''; break;
      case 0x93:
      case 0x94: out += '"'; break;
      case 0x96:
      case 0x97: out += '-'; break;
      default: break;
    }
    ++i;
  }
  return out;
}

bool parse_xstr(std::span<const uint8_t> block, std::map<uint16_t, std::string>& strings) {
  strings.clear();
  if (block.size() < 14 || u32_be(block, 0) != kXstrMagic) return false;
  const uint16_t count = u16_be(block, 12);
  size_t pos = 14;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 4 > block.size()) break;
    const uint16_t sid = u16_be(block, pos);
    const uint16_t slen = u16_be(block, pos + 2);
    pos += 4;
    if (pos + slen > block.size()) break;
    const std::string_view raw(reinterpret_cast<const char*>(block.data() + pos), slen);
    strings[sid] = clean_spa_text(raw);
    pos += slen;
  }
  return !strings.empty();
}

}  // namespace

uint16_t spa_language_id(std::string_view code) {
  std::string up;
  for (char c : code) up += char(std::toupper(static_cast<unsigned char>(c)));
  if (up == "JA" || up == "JP") return uint16_t(SpaLanguage::kJapanese);
  if (up == "DE") return uint16_t(SpaLanguage::kGerman);
  if (up == "FR") return uint16_t(SpaLanguage::kFrench);
  if (up == "ES" || up == "MX") return uint16_t(SpaLanguage::kSpanish);
  if (up == "IT") return uint16_t(SpaLanguage::kItalian);
  if (up == "KO" || up == "KR") return uint16_t(SpaLanguage::kKorean);
  if (up == "ZH" || up == "ZH-HANT" || up == "ZHT" || up == "TC") return uint16_t(SpaLanguage::kTraditionalChinese);
  if (up == "PT" || up == "BR") return uint16_t(SpaLanguage::kPortuguese);
  if (up == "PL") return uint16_t(SpaLanguage::kPolish);
  if (up == "RU") return uint16_t(SpaLanguage::kRussian);
  if (up == "SV") return uint16_t(SpaLanguage::kSwedish);
  if (up == "NB" || up == "NO") return uint16_t(SpaLanguage::kNorwegian);
  if (up == "NL") return uint16_t(SpaLanguage::kDutch);
  return uint16_t(SpaLanguage::kEnglish);
}

const SpaAchievement* SpaDatabase::find(uint16_t id) const {
  for (const auto& a : achievements)
    if (a.id == id) return &a;
  return nullptr;
}

const std::vector<uint8_t>* SpaDatabase::find_image(uint32_t image_id) const {
  const auto it = images.find(image_id);
  return it != images.end() ? &it->second : nullptr;
}

void SpaDatabase::merge(const SpaDatabase& other) {
  if (title_id == 0 && other.title_id != 0) title_id = other.title_id;
  for (const auto& oa : other.achievements) {
    bool found = false;
    for (auto& ea : achievements) {
      if (ea.id == oa.id) {
        if (ea.label.empty() && !oa.label.empty()) ea.label = oa.label;
        if (ea.description.empty() && !oa.description.empty()) ea.description = oa.description;
        if (ea.unachieved_description.empty() && !oa.unachieved_description.empty())
          ea.unachieved_description = oa.unachieved_description;
        if (ea.image_id == 0 && oa.image_id != 0) ea.image_id = oa.image_id;
        if (ea.gamerscore == 0 && oa.gamerscore != 0) ea.gamerscore = oa.gamerscore;
        found = true;
        break;
      }
    }
    if (!found) achievements.push_back(oa);
  }
  for (const auto& [img_id, bytes] : other.images) {
    if (!images.count(img_id) && !bytes.empty()) images[img_id] = bytes;
  }
  std::sort(achievements.begin(), achievements.end(),
            [](const SpaAchievement& a, const SpaAchievement& b) { return a.id < b.id; });
}

bool parse_spa_xdbf(std::span<const uint8_t> bytes, SpaDatabase& out, uint16_t language_id, std::string* error) {
  out = SpaDatabase{};
  if (bytes.size() < 24) {
    if (error) *error = "XDBF buffer smaller than 24-byte header";
    return false;
  }
  if (u32_be(bytes, 0) != kXdbfMagic) {
    if (error) *error = "invalid XDBF magic";
    return false;
  }
  const uint32_t entry_table_len = u32_be(bytes, 8);
  const uint32_t entry_count = u32_be(bytes, 12);
  const uint32_t free_table_len = u32_be(bytes, 16);
  if (entry_count > entry_table_len || entry_table_len > 65536u || free_table_len > 65536u) {
    if (error) *error = "XDBF table counts out of bounds";
    return false;
  }
  const size_t data_offset = 24u + size_t(entry_table_len) * 18u + size_t(free_table_len) * 8u;
  if (data_offset > bytes.size()) {
    if (error) *error = "XDBF entry/free tables extend past EOF";
    return false;
  }

  struct Entry {
    uint16_t section;
    uint64_t id;
    uint32_t offset;
    uint32_t size;
  };
  std::vector<Entry> entries;
  entries.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    const size_t eo = 24u + size_t(i) * 18u;
    Entry e{};
    e.section = u16_be(bytes, eo);
    e.id = read_u64_be(bytes, eo + 2);
    e.offset = u32_be(bytes, eo + 10);
    e.size = u32_be(bytes, eo + 14);
    if (e.size > 0 && size_t(e.offset) + size_t(e.size) <= bytes.size() - data_offset) {
      entries.push_back(e);
    }
  }

  auto find_block = [&](uint16_t sec, uint64_t id) -> std::span<const uint8_t> {
    for (const auto& e : entries) {
      if (e.section == sec && e.id == id) {
        return bytes.subspan(data_offset + size_t(e.offset), size_t(e.size));
      }
    }
    return {};
  };

  if (const auto xthd = find_block(kSectionMetadata, kXthdMagic); xthd.size() >= 16 && u32_be(xthd, 0) == kXthdMagic) {
    out.title_id = u32_be(xthd, 12);
  }
  if (const auto xstc = find_block(kSectionMetadata, kXstcMagic); xstc.size() >= 16 && u32_be(xstc, 0) == kXstcMagic) {
    out.default_language = uint16_t(u32_be(xstc, 12));
  }

  // Resolve localized string table with fallback to default language -> English -> first available.
  std::map<uint16_t, std::string> strings;
  auto try_lang = [&](uint16_t lid) {
    if (const auto blk = find_block(kSectionStringTable, lid); !blk.empty()) return parse_xstr(blk, strings);
    return false;
  };
  if (!try_lang(language_id) && !try_lang(out.default_language) && !try_lang(uint16_t(SpaLanguage::kEnglish))) {
    for (const auto& e : entries) {
      if (e.section == kSectionStringTable && parse_xstr(bytes.subspan(data_offset + e.offset, e.size), strings)) break;
    }
  }

  // Parse XACH achievement table (14-byte header + N * 36-byte records).
  const auto xach = find_block(kSectionMetadata, kXachMagic);
  if (xach.size() >= 14 && u32_be(xach, 0) == kXachMagic) {
    const uint16_t count = u16_be(xach, 12);
    out.achievements.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
      const size_t ro = 14u + size_t(i) * 36u;
      if (ro + 36u > xach.size()) break;
      SpaAchievement a{};
      a.id = u16_be(xach, ro + 0);
      a.label_id = u16_be(xach, ro + 2);
      a.description_id = u16_be(xach, ro + 4);
      a.unachieved_id = u16_be(xach, ro + 6);
      a.image_id = u32_be(xach, ro + 8);
      a.gamerscore = u16_be(xach, ro + 12);
      a.flags = u32_be(xach, ro + 16);
      if (auto it = strings.find(a.label_id); it != strings.end()) a.label = it->second;
      if (auto it = strings.find(a.description_id); it != strings.end()) a.description = it->second;
      if (auto it = strings.find(a.unachieved_id); it != strings.end()) a.unachieved_description = it->second;
      out.achievements.push_back(std::move(a));
    }
  }

  // Extract PNG image blobs from section 2.
  for (const auto& e : entries) {
    if (e.section != kSectionImage || e.size < 8) continue;
    const auto blk = bytes.subspan(data_offset + size_t(e.offset), size_t(e.size));
    out.images[uint32_t(e.id)].assign(blk.begin(), blk.end());
  }

  if (out.achievements.empty()) {
    if (error) *error = "XDBF contains no XACH achievements";
    return false;
  }
  return true;
}

bool load_spa_file(const std::filesystem::path& path, SpaDatabase& out, uint16_t language_id, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse_spa_xdbf(data, out, language_id, error);
}

size_t load_spa_packages(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                         SpaDatabase& out, std::string_view language, std::string* note) {
  const uint16_t lid = spa_language_id(language);
  std::vector<std::filesystem::path> candidates;
  std::error_code ec;

  auto collect_spa_in = [&](const std::filesystem::path& root, int max_depth) {
    if (root.empty() || !std::filesystem::exists(root, ec)) return;
    if (std::filesystem::is_regular_file(root, ec)) {
      candidates.push_back(root);
      return;
    }
    if (std::filesystem::exists(root / "spa.bin", ec)) candidates.push_back(root / "spa.bin");
    if (max_depth <= 0) return;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
      if (!entry.is_directory(ec)) continue;
      if (std::filesystem::exists(entry.path() / "spa.bin", ec)) {
        candidates.push_back(entry.path() / "spa.bin");
      }
    }
  };

  if (!game_root.empty()) {
    collect_spa_in(game_root, 1);
    collect_spa_in(game_root / "dlc", 1);
  }
  if (!state_root.empty()) {
    collect_spa_in(state_root, 1);
    collect_spa_in(state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002", 1);
    collect_spa_in(state_root / "0000000000000000" / "4D5309C9" / "00000002", 1);
    collect_spa_in(state_root / "4D5309C9" / "00000002", 1);
    collect_spa_in(state_root / "00000002", 1);
    collect_spa_in(state_root / "dlc", 1);
  }

  // Deduplicate and sort so larger packages (e.g. 1000 Club 70-entry) merge cleanly.
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

  size_t loaded = 0;
  for (const auto& p : candidates) {
    SpaDatabase pkg;
    if (load_spa_file(p, pkg, lid, nullptr)) {
      out.merge(pkg);
      ++loaded;
    }
  }
  if (note) {
    *note = std::to_string(loaded) + " spa.bin package(s), " + std::to_string(out.achievements.size()) +
            " achievements, " + std::to_string(out.images.size()) + " icons";
  }
  return loaded;
}

}  // namespace fh1
