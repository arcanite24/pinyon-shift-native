#include "setup/mod_installer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>

#include "cars/mod_manager.h"
#include "fh1data/archive.h"
#include "fh1data/bytes.h"
#include "fh1data/inflate.h"

namespace setup::mods {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kZipLocalSig = 0x04034B50u;
constexpr uint32_t kZipCentralSig = 0x02014B50u;
constexpr uint32_t kZipEocdSig = 0x06054B50u;
constexpr size_t kZipEocdMin = 22;
constexpr size_t kZipCentralMin = 46;
constexpr size_t kZipLocalMin = 30;
constexpr uint64_t kMaxMemberBytes = 256ull * 1024ull * 1024ull;

struct ZipMember {
  std::string raw_name;
  std::string clean_path;
  uint16_t method = 0;
  uint32_t crc32 = 0;
  uint32_t comp_size = 0;
  uint32_t uncomp_size = 0;
  uint32_t local_offset = 0;
  bool is_dir = false;
};

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string sanitize_mod_folder_name(std::string_view raw) {
  std::string out;
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
      out.push_back(c);
    } else if (c == ' ') {
      out.push_back('_');
    }
  }
  while (!out.empty() && (out.front() == '.' || out.front() == '_')) out.erase(out.begin());
  while (!out.empty() && out.back() == '.') out.pop_back();
  if (out.empty()) out = "mod";
  return out;
}

bool parse_zip_directory(const StreamReader& reader, std::vector<ZipMember>& out, std::string* error) {
  out.clear();
  const uint64_t total = reader.size();
  if (total < kZipEocdMin) {
    if (error) *error = "file too small for a ZIP archive";
    return false;
  }

  const size_t tail_len = static_cast<size_t>(std::min<uint64_t>(total, kZipEocdMin + 0xFFFFu));
  std::vector<uint8_t> tail;
  if (!reader.read_at(total - tail_len, tail_len, tail, error)) return false;

  size_t eocd = SIZE_MAX;
  for (size_t pos = tail_len - kZipEocdMin + 1; pos-- > 0;) {
    if (fh1::load_le32(tail.data() + pos) == kZipEocdSig) {
      eocd = pos;
      break;
    }
  }
  if (eocd == SIZE_MAX) {
    if (error) *error = "invalid ZIP archive (no end-of-central-directory record)";
    return false;
  }

  const uint32_t cd_size = fh1::load_le32(tail.data() + eocd + 12);
  const uint32_t cd_offset = fh1::load_le32(tail.data() + eocd + 16);
  if (cd_offset == 0xFFFFFFFFu || static_cast<uint64_t>(cd_offset) + cd_size > total) {
    if (error) *error = "ZIP central directory out of bounds";
    return false;
  }

  std::vector<uint8_t> cd;
  if (!reader.read_at(cd_offset, cd_size, cd, error)) return false;

  size_t pos = 0;
  while (pos < cd.size()) {
    if (cd.size() - pos < kZipCentralMin || fh1::load_le32(cd.data() + pos) != kZipCentralSig) {
      if (error) *error = "corrupt ZIP central directory entry";
      return false;
    }
    const uint8_t* h = cd.data() + pos;
    const uint16_t method = fh1::load_le16(h + 10);
    const uint32_t crc = fh1::load_le32(h + 16);
    const uint32_t comp = fh1::load_le32(h + 20);
    const uint32_t uncomp = fh1::load_le32(h + 24);
    const uint16_t name_len = fh1::load_le16(h + 28);
    const uint16_t extra_len = fh1::load_le16(h + 30);
    const uint16_t comment_len = fh1::load_le16(h + 32);
    const uint32_t local_off = fh1::load_le32(h + 42);
    if (cd.size() - pos - kZipCentralMin < static_cast<size_t>(name_len) + extra_len + comment_len) {
      if (error) *error = "truncated ZIP central directory entry";
      return false;
    }

    ZipMember m;
    m.raw_name.assign(reinterpret_cast<const char*>(h + kZipCentralMin), name_len);
    m.is_dir = !m.raw_name.empty() && (m.raw_name.back() == '/' || m.raw_name.back() == '\\');
    m.clean_path = sanitize_relative_path(m.raw_name);
    if (!m.raw_name.empty() && m.clean_path.empty()) {
      if (error) *error = "rejected unsafe ZIP path: " + m.raw_name;
      return false;
    }
    m.method = method;
    m.crc32 = crc;
    m.comp_size = comp;
    m.uncomp_size = uncomp;
    m.local_offset = local_off;
    out.push_back(std::move(m));

    pos += kZipCentralMin + name_len + extra_len + comment_len;
  }
  return true;
}

bool read_zip_member(const StreamReader& reader, const ZipMember& m, std::vector<uint8_t>& out, std::string* error) {
  out.clear();
  if (m.is_dir) return true;
  if (m.uncomp_size > kMaxMemberBytes) {
    if (error) *error = "ZIP member exceeds 256 MiB safety cap: " + m.clean_path;
    return false;
  }

  uint8_t lhdr[kZipLocalMin]{};
  if (!reader.read_at(m.local_offset, lhdr, sizeof(lhdr), error)) return false;
  if (fh1::load_le32(lhdr) != kZipLocalSig) {
    if (error) *error = "invalid ZIP local file header for " + m.clean_path;
    return false;
  }
  const uint16_t lname_len = fh1::load_le16(lhdr + 26);
  const uint16_t lextra_len = fh1::load_le16(lhdr + 28);
  const uint64_t payload_off = static_cast<uint64_t>(m.local_offset) + kZipLocalMin + lname_len + lextra_len;

  std::vector<uint8_t> comp;
  if (!reader.read_at(payload_off, m.comp_size, comp, error)) return false;

  if (m.method == 0) {
    if (comp.size() != m.uncomp_size) {
      if (error) *error = "ZIP stored member size mismatch: " + m.clean_path;
      return false;
    }
    out = std::move(comp);
  } else if (m.method == 8) {
    auto inflated = fh1::inflate_raw(comp, m.uncomp_size);
    if (!inflated) {
      if (error) *error = "ZIP deflate decode failed: " + m.clean_path;
      return false;
    }
    out = std::move(*inflated);
  } else {
    if (error) *error = "unsupported ZIP compression method " + std::to_string(m.method) + " on " + m.clean_path;
    return false;
  }

  if (m.crc32 != 0u && fh1::crc32(out) != m.crc32) {
    if (error) *error = "ZIP CRC-32 mismatch on " + m.clean_path;
    return false;
  }
  return true;
}

// If every file in the ZIP lives under a single top-level directory (e.g.
// "my_mod/mod.toml", "my_mod/textures/..."), returns "my_mod" so extraction
// avoids double-nesting mods/my_mod/my_mod/. Does NOT unwrap standard Shift
// Engine mod content subdirectories (db/, textures/, bake/, media/, etc.).
bool is_reserved_mod_subdir(std::string_view dir) {
  static const std::set<std::string> kReserved = {
      "bake", "sound", "media", "radio", "game", "db", "members",
      "xml_patches", "merge", "stringtables", "strings", "textures", "shaders", "code",
  };
  return kReserved.count(lower_ascii(dir)) != 0;
}

std::string detect_single_root_folder(const std::vector<ZipMember>& members) {
  std::string root;
  bool saw_file = false;
  for (const auto& m : members) {
    if (m.is_dir || m.clean_path.empty()) continue;
    saw_file = true;
    const size_t slash = m.clean_path.find('/');
    if (slash == std::string::npos) return {};
    const std::string top = m.clean_path.substr(0, slash);
    if (root.empty()) {
      root = top;
    } else if (root != top) {
      return {};
    }
  }
  if (!saw_file || is_reserved_mod_subdir(root)) return {};
  return root;
}

void ensure_mod_manifest(const fs::path& mod_dir, const std::string& mod_name) {
  std::error_code ec;
  if (fs::is_regular_file(mod_dir / "mod.toml", ec) || fs::is_regular_file(mod_dir / "mod.txt", ec)) return;
  std::ofstream f(mod_dir / "mod.toml", std::ios::binary | std::ios::trunc);
  f << "name = \"" << mod_name << "\"\n"
    << "title = \"" << mod_name << "\"\n"
    << "version = \"1.0.0\"\n"
    << "enabled = true\n";
}

void sync_config_order(const fs::path& mods_root, const std::vector<cars::modmgr::ModInfo>& scanned,
                       cars::modmgr::Config& cfg) {
  std::set<std::string> known;
  for (const auto& m : scanned) known.insert(m.name);

  std::vector<std::string> cleaned;
  std::set<std::string> seen;
  for (const auto& name : cfg.order) {
    if (known.count(name) && seen.insert(name).second) cleaned.push_back(name);
  }
  for (const auto& m : scanned) {
    if (seen.insert(m.name).second) cleaned.push_back(m.name);
  }
  cfg.order = std::move(cleaned);
  (void)mods_root;
}

bool has_elf_magic(const uint8_t* p, size_t n) {
  return n >= 4 && p[0] == 0x7Fu && p[1] == 'E' && p[2] == 'L' && p[3] == 'F';
}

}  // namespace

bool install_mod_zip(const StreamReader& reader, const std::string& suggested_name, const fs::path& mods_root,
                     const ProgressCallback& progress, const std::atomic<bool>* cancel, InstallModReport& report) {
  report = InstallModReport{};
  std::string err;
  std::vector<ZipMember> members;
  if (!parse_zip_directory(reader, members, &err)) {
    report.error = err;
    return false;
  }

  const std::string single_root = detect_single_root_folder(members);
  const std::string raw_mod_name = !single_root.empty() ? single_root : suggested_name;
  const std::string mod_name = sanitize_mod_folder_name(raw_mod_name);
  const fs::path mod_dir = mods_root / mod_name;

  std::error_code ec;
  fs::create_directories(mod_dir, ec);
  if (ec) {
    report.error = "cannot create mod directory: " + ec.message();
    return false;
  }

  std::vector<const ZipMember*> files;
  uint64_t total_bytes = 0;
  for (const auto& m : members) {
    if (m.is_dir || m.clean_path.empty()) continue;
    files.push_back(&m);
    total_bytes += m.uncomp_size;
  }

  uint64_t bytes_done = 0;
  size_t files_done = 0;
  std::vector<uint8_t> payload;

  for (const ZipMember* m : files) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      report.cancelled = true;
      report.error = "mod install cancelled";
      return false;
    }

    std::string rel = m->clean_path;
    if (!single_root.empty() && rel.size() > single_root.size() + 1 &&
        rel.compare(0, single_root.size(), single_root) == 0 && rel[single_root.size()] == '/') {
      rel = rel.substr(single_root.size() + 1);
    }
    rel = sanitize_relative_path(rel);
    if (rel.empty()) continue;

    if (!read_zip_member(reader, *m, payload, &err)) {
      report.error = err;
      return false;
    }

    const fs::path dest = mod_dir / fs::path(rel);
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
      report.error = "cannot write mod file: " + dest.string();
      return false;
    }
    if (!payload.empty()) {
      out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
      if (!out) {
        report.error = "write error on mod file: " + dest.string();
        return false;
      }
    }
    out.close();

    bytes_done += payload.size();
    report.bytes_written += payload.size();
    ++files_done;
    ++report.files_extracted;
    if (progress) progress({files_done, files.size(), bytes_done, total_bytes, rel});
  }

  ensure_mod_manifest(mod_dir, mod_name);

  // Register the new mod in mods.toml as enabled.
  auto scanned = cars::modmgr::scan(mods_root);
  cars::modmgr::Config cfg;
  cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg);
  sync_config_order(mods_root, scanned, cfg);
  cfg.enabled[mod_name] = true;
  cars::modmgr::write_config(cars::modmgr::config_path(mods_root), cfg, nullptr);

  report.ok = true;
  report.mod_name = mod_name;
  report.mod_dir = mod_dir;
  return true;
}

bool install_mod_zip_path(const fs::path& zip_path, const fs::path& mods_root, const ProgressCallback& progress,
                          const std::atomic<bool>* cancel, InstallModReport& report) {
  std::string err;
  auto reader = StreamReader::open_path(zip_path, &err);
  if (!reader) {
    report = InstallModReport{};
    report.error = err;
    return false;
  }
  return install_mod_zip(*reader, zip_path.stem().string(), mods_root, progress, cancel, report);
}

bool install_mod_zip_fd(int fd, bool take_ownership, const std::string& suggested_name, const fs::path& mods_root,
                        const ProgressCallback& progress, const std::atomic<bool>* cancel, InstallModReport& report) {
  std::string err;
  auto reader = StreamReader::from_fd(fd, take_ownership, &err);
  if (!reader) {
    report = InstallModReport{};
    report.error = err;
    return false;
  }
  return install_mod_zip(*reader, suggested_name, mods_root, progress, cancel, report);
}

bool install_mod_directory(const fs::path& src_dir, const fs::path& mods_root, InstallModReport& report) {
  report = InstallModReport{};
  std::error_code ec;
  if (!fs::is_directory(src_dir, ec)) {
    report.error = "mod directory does not exist: " + src_dir.string();
    return false;
  }
  const std::string mod_name = sanitize_mod_folder_name(src_dir.filename().string());
  const fs::path dest = mods_root / mod_name;
  fs::create_directories(dest, ec);
  if (ec) {
    report.error = "cannot create mod directory: " + ec.message();
    return false;
  }
  for (const auto& entry : fs::recursive_directory_iterator(src_dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const auto rel = fs::relative(entry.path(), src_dir, ec);
    const std::string clean = sanitize_relative_path(rel.generic_string());
    if (clean.empty()) continue;
    const fs::path target = dest / fs::path(clean);
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      report.error = "failed copying mod file " + clean + ": " + ec.message();
      return false;
    }
    ++report.files_extracted;
    report.bytes_written += fs::file_size(target, ec);
  }
  ensure_mod_manifest(dest, mod_name);

  auto scanned = cars::modmgr::scan(mods_root);
  cars::modmgr::Config cfg;
  cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg);
  sync_config_order(mods_root, scanned, cfg);
  cfg.enabled[mod_name] = true;
  cars::modmgr::write_config(cars::modmgr::config_path(mods_root), cfg, nullptr);

  report.ok = true;
  report.mod_name = mod_name;
  report.mod_dir = dest;
  return true;
}

std::vector<InstalledMod> list_mods(const fs::path& mods_root) {
  std::vector<InstalledMod> out;
  std::error_code ec;
  if (mods_root.empty() || !fs::is_directory(mods_root, ec)) return out;

  const auto scanned = cars::modmgr::scan(mods_root);
  cars::modmgr::Config cfg;
  cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg);
  sync_config_order(mods_root, scanned, cfg);

  std::map<std::string, const cars::modmgr::ModInfo*> by_name;
  for (const auto& m : scanned) by_name[m.name] = &m;

  for (size_t i = 0; i < cfg.order.size(); ++i) {
    const auto it = by_name.find(cfg.order[i]);
    if (it == by_name.end()) continue;
    const auto& m = *it->second;
    InstalledMod im;
    im.name = m.name;
    im.title = !m.manifest.title.empty() ? m.manifest.title : m.name;
    im.version = m.manifest.version;
    im.author = m.manifest.author;
    im.summary = m.content.summary();
    im.priority = m.manifest.priority;
    im.order_index = i;
    im.dir = m.dir;
    auto en = cfg.enabled.find(m.name);
    im.enabled = (en != cfg.enabled.end()) ? en->second : m.manifest.enabled;
    out.push_back(std::move(im));
  }
  return out;
}

bool set_mod_enabled(const fs::path& mods_root, const std::string& mod_name, bool enabled, std::string* error) {
  const auto scanned = cars::modmgr::scan(mods_root);
  bool found = false;
  for (const auto& m : scanned) {
    if (m.name == mod_name) found = true;
  }
  if (!found) {
    if (error) *error = "mod not found: " + mod_name;
    return false;
  }
  cars::modmgr::Config cfg;
  cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg);
  sync_config_order(mods_root, scanned, cfg);
  cfg.enabled[mod_name] = enabled;
  return cars::modmgr::write_config(cars::modmgr::config_path(mods_root), cfg, error);
}

bool move_mod(const fs::path& mods_root, const std::string& mod_name, int delta, std::string* error) {
  const auto scanned = cars::modmgr::scan(mods_root);
  cars::modmgr::Config cfg;
  cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg);
  sync_config_order(mods_root, scanned, cfg);

  const auto it = std::find(cfg.order.begin(), cfg.order.end(), mod_name);
  if (it == cfg.order.end()) {
    if (error) *error = "mod not found: " + mod_name;
    return false;
  }
  const int cur = static_cast<int>(std::distance(cfg.order.begin(), it));
  const int target = std::clamp(cur + delta, 0, static_cast<int>(cfg.order.size()) - 1);
  if (target == cur) return true;
  std::string moved = std::move(cfg.order[static_cast<size_t>(cur)]);
  cfg.order.erase(cfg.order.begin() + cur);
  cfg.order.insert(cfg.order.begin() + target, std::move(moved));
  return cars::modmgr::write_config(cars::modmgr::config_path(mods_root), cfg, error);
}

bool remove_mod(const fs::path& mods_root, const std::string& mod_name, std::string* error) {
  const std::string clean = sanitize_relative_path(mod_name);
  if (clean.empty() || clean.find('/') != std::string::npos) {
    if (error) *error = "invalid mod name: " + mod_name;
    return false;
  }
  std::error_code ec;
  const fs::path target = mods_root / clean;
  if (!fs::exists(target, ec)) {
    if (error) *error = "mod folder does not exist: " + clean;
    return false;
  }
  fs::remove_all(target, ec);
  if (ec) {
    if (error) *error = "failed removing mod: " + ec.message();
    return false;
  }
  const auto scanned = cars::modmgr::scan(mods_root);
  cars::modmgr::Config cfg;
  cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg);
  cfg.enabled.erase(clean);
  sync_config_order(mods_root, scanned, cfg);
  return cars::modmgr::write_config(cars::modmgr::config_path(mods_root), cfg, error);
}

bool install_turnip_driver(const StreamReader& reader, const std::string& filename, const fs::path& drivers_root,
                           TurnipInstallReport& report) {
  report = TurnipInstallReport{};
  const uint64_t sz = reader.size();
  if (sz < 4) {
    report.error = "driver file is empty or too small";
    return false;
  }

  uint8_t magic[4]{};
  std::string err;
  if (!reader.read_at(0, magic, sizeof(magic), &err)) {
    report.error = err;
    return false;
  }

  const fs::path dest_dir = drivers_root / "turnip";
  std::error_code ec;
  fs::create_directories(dest_dir, ec);
  if (ec) {
    report.error = "cannot create drivers/turnip directory: " + ec.message();
    return false;
  }
  const fs::path dest_so = dest_dir / "libvulkan_freedreno.so";

  // Case 1: Raw ELF .so driver
  if (has_elf_magic(magic, sizeof(magic))) {
    if (sz > kMaxMemberBytes) {
      report.error = "driver .so exceeds size limit";
      return false;
    }
    std::vector<uint8_t> data;
    if (!reader.read_at(0, static_cast<size_t>(sz), data, &err)) {
      report.error = err;
      return false;
    }
    std::ofstream out(dest_so, std::ios::binary | std::ios::trunc);
    if (!out) {
      report.error = "cannot write " + dest_so.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) {
      report.error = "failed writing Turnip .so";
      return false;
    }
    report.ok = true;
    report.driver_path = dest_so;
    report.driver_name = filename.empty() ? "libvulkan_freedreno.so" : filename;
    report.bytes_written = data.size();
    return true;
  }

  // Case 2: ZIP archive containing a Turnip .so (and optional meta.json / NOTICE.txt)
  if (fh1::load_le32(magic) == kZipLocalSig) {
    std::vector<ZipMember> members;
    if (!parse_zip_directory(reader, members, &err)) {
      report.error = err;
      return false;
    }
    const ZipMember* so_member = nullptr;
    for (const auto& m : members) {
      if (m.is_dir) continue;
      const std::string nl = lower_ascii(m.clean_path);
      if (nl.size() >= 3 && nl.compare(nl.size() - 3, 3, ".so") == 0) {
        if (!so_member || nl.find("freedreno") != std::string::npos || nl.find("turnip") != std::string::npos) {
          so_member = &m;
        }
      }
    }
    if (!so_member) {
      report.error = "ZIP does not contain a Vulkan driver .so library";
      return false;
    }
    std::vector<uint8_t> so_bytes;
    if (!read_zip_member(reader, *so_member, so_bytes, &err)) {
      report.error = err;
      return false;
    }
    if (!has_elf_magic(so_bytes.data(), so_bytes.size())) {
      report.error = "extracted driver is not a valid ELF shared library";
      return false;
    }
    std::ofstream out(dest_so, std::ios::binary | std::ios::trunc);
    if (!out) {
      report.error = "cannot write " + dest_so.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(so_bytes.data()), static_cast<std::streamsize>(so_bytes.size()));
    if (!out) {
      report.error = "failed writing Turnip .so";
      return false;
    }
    out.close();

    // Extract optional metadata files beside the .so
    for (const auto& m : members) {
      if (m.is_dir) continue;
      const std::string fname = lower_ascii(fs::path(m.clean_path).filename().string());
      if (fname == "meta.json" || fname == "notice.txt") {
        std::vector<uint8_t> extra;
        if (read_zip_member(reader, m, extra, nullptr)) {
          std::ofstream ef(dest_dir / fs::path(m.clean_path).filename(), std::ios::binary | std::ios::trunc);
          ef.write(reinterpret_cast<const char*>(extra.data()), static_cast<std::streamsize>(extra.size()));
        }
      }
    }

    report.ok = true;
    report.driver_path = dest_so;
    report.driver_name = fs::path(so_member->clean_path).filename().string();
    report.bytes_written = so_bytes.size();
    return true;
  }

  report.error = "unsupported driver file (expected ELF .so or Turnip .zip archive)";
  return false;
}

bool install_turnip_driver_path(const fs::path& path, const fs::path& drivers_root, TurnipInstallReport& report) {
  std::string err;
  auto reader = StreamReader::open_path(path, &err);
  if (!reader) {
    report = TurnipInstallReport{};
    report.error = err;
    return false;
  }
  return install_turnip_driver(*reader, path.filename().string(), drivers_root, report);
}

bool install_turnip_driver_fd(int fd, bool take_ownership, const std::string& filename, const fs::path& drivers_root,
                              TurnipInstallReport& report) {
  std::string err;
  auto reader = StreamReader::from_fd(fd, take_ownership, &err);
  if (!reader) {
    report = TurnipInstallReport{};
    report.error = err;
    return false;
  }
  return install_turnip_driver(*reader, filename, drivers_root, report);
}

fs::path find_installed_turnip_driver(const fs::path& drivers_root) {
  std::error_code ec;
  if (drivers_root.empty() || !fs::is_directory(drivers_root, ec)) return {};
  const fs::path direct = drivers_root / "turnip" / "libvulkan_freedreno.so";
  if (fs::is_regular_file(direct, ec)) return direct;
  for (const auto& e : fs::directory_iterator(drivers_root, ec)) {
    if (!e.is_directory(ec)) continue;
    if (lower_ascii(e.path().filename().string()).find("turnip") == std::string::npos) continue;
    const fs::path so = e.path() / "libvulkan_freedreno.so";
    if (fs::is_regular_file(so, ec)) return so;
  }
  return {};
}

}  // namespace setup::mods
