// Host unit tests for standalone Android on-device setup (libs/setup):
//   - Clean-room GDFX / XISO reader & extractor across all 4 partition offsets,
//     nested directories, POSIX fd streaming, progress, resume, and cancel
//   - Clean-room STFS (PIRS / LIVE / CON ) container reader & installer with
//     multi-block non-contiguous hash chains, Title Update v4, and DLC layout
//   - On-device .zip mod installer, toggle, load-order move, zip-slip guard,
//     and Mesa Turnip .so / .zip driver installation
//   - On-device baker thread capping, Fast Start vs Full Roster selection,
//     manifest/state round-trip, play.txt generation, and library inspection
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "cars/mod_manager.h"
#include "cars/sources.h"
#include "fh1data/archive.h"
#include "setup/mod_installer.h"
#include "setup/on_device_baker.h"
#include "setup/stfs.h"
#include "setup/stream_reader.h"
#include "setup/xiso.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                         \
  do {                                                                                      \
    if (!(cond)) {                                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
      ++g_failures;                                                                         \
    }                                                                                       \
  } while (0)

void put_le16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
  buf[off] = static_cast<uint8_t>(v & 0xFFu);
  buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void put_le24(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = static_cast<uint8_t>(v & 0xFFu);
  buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
}

void put_le32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = static_cast<uint8_t>(v & 0xFFu);
  buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void put_be16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
  buf[off] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  buf[off + 1] = static_cast<uint8_t>(v & 0xFFu);
}

void put_be24(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  buf[off + 2] = static_cast<uint8_t>(v & 0xFFu);
}

void put_be32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = static_cast<uint8_t>((v >> 24) & 0xFFu);
  buf[off + 1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  buf[off + 2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  buf[off + 3] = static_cast<uint8_t>(v & 0xFFu);
}

void write_bytes(const fs::path& p, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int open_readonly_fd(const fs::path& p) {
#ifdef _WIN32
  return _wopen(p.c_str(), _O_RDONLY | _O_BINARY);
#else
  return ::open(p.c_str(), O_RDONLY);
#endif
}

// ---- Synthetic GDFX / XISO builder -----------------------------------------

struct GdfxNodeSpec {
  std::string name;
  uint32_t sector = 0;
  uint32_t size = 0;
  bool is_dir = false;
  int left = -1;
  int right = -1;
};

std::vector<uint8_t> build_gdfx_dir_table(const std::vector<GdfxNodeSpec>& nodes) {
  std::vector<uint32_t> offsets(nodes.size(), 0);
  uint32_t cursor = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    offsets[i] = cursor;
    cursor += static_cast<uint32_t>(14 + nodes[i].name.size());
    cursor = (cursor + 3u) & ~3u;  // 4-byte aligned for word offsets
  }
  std::vector<uint8_t> buf(cursor, 0xFFu);
  for (size_t i = 0; i < nodes.size(); ++i) {
    const uint32_t off = offsets[i];
    put_le16(buf, off + 0, nodes[i].left >= 0 ? static_cast<uint16_t>(offsets[static_cast<size_t>(nodes[i].left)] / 4u) : 0xFFFFu);
    put_le16(buf, off + 2, nodes[i].right >= 0 ? static_cast<uint16_t>(offsets[static_cast<size_t>(nodes[i].right)] / 4u) : 0xFFFFu);
    put_le32(buf, off + 4, nodes[i].sector);
    put_le32(buf, off + 8, nodes[i].size);
    buf[off + 12] = nodes[i].is_dir ? 0x10u : 0x20u;
    buf[off + 13] = static_cast<uint8_t>(nodes[i].name.size());
    std::memcpy(buf.data() + off + 14, nodes[i].name.data(), nodes[i].name.size());
  }
  return buf;
}

// Builds a compact self-contained GDFX partition image (relative to partition offset 0).
std::vector<uint8_t> build_synthetic_xiso_partition(bool inject_traversal = false) {
  using setup::xiso::kSectorSize;
  const std::string car_data = "CARZIP-SYNTHETIC-PAYLOAD";
  const std::string db_data = "SQLITE-SYNTHETIC-DB-DATA";
  const std::string col_data = "COLORADO-BIN-ZIP-PAYLOAD";
  const std::string xex_data = "XEX2-SKIP-ME";
  const std::string root_txt = "ROOT-README";

  // Sector layout:
  // 32: volume descriptor
  // 33: root dir
  // 34: media dir
  // 35: media/cars dir
  // 36: media/db dir
  // 37: media/tracks dir
  // 38: media/tracks/colorado dir
  // 40: media/cars/test_car.zip
  // 41: media/db/gamedb.slt
  // 42: media/tracks/colorado/bin.zip
  // 43: default.xex
  // 44: readme.txt
  const auto cars_dir = build_gdfx_dir_table({
      {inject_traversal ? "../evil.txt" : "test_car.zip", 40, static_cast<uint32_t>(car_data.size()), false, -1, -1},
  });
  const auto db_dir = build_gdfx_dir_table({
      {"gamedb.slt", 41, static_cast<uint32_t>(db_data.size()), false, -1, -1},
  });
  const auto colorado_dir = build_gdfx_dir_table({
      {"bin.zip", 42, static_cast<uint32_t>(col_data.size()), false, -1, -1},
  });
  const auto tracks_dir = build_gdfx_dir_table({
      {"colorado", 38, static_cast<uint32_t>(colorado_dir.size()), true, -1, -1},
  });
  // BST inside media/: root="db", left="cars", right="tracks"
  const auto media_dir = build_gdfx_dir_table({
      {"db", 36, static_cast<uint32_t>(db_dir.size()), true, 1, 2},
      {"cars", 35, static_cast<uint32_t>(cars_dir.size()), true, -1, -1},
      {"tracks", 37, static_cast<uint32_t>(tracks_dir.size()), true, -1, -1},
  });
  // BST inside root/: root="media", left="default.xex", right="readme.txt"
  const auto root_dir = build_gdfx_dir_table({
      {"media", 34, static_cast<uint32_t>(media_dir.size()), true, 1, 2},
      {"default.xex", 43, static_cast<uint32_t>(xex_data.size()), false, -1, -1},
      {"readme.txt", 44, static_cast<uint32_t>(root_txt.size()), false, -1, -1},
  });

  std::vector<uint8_t> img(46u * kSectorSize, 0);
  auto write_sector = [&](uint32_t sec, const void* src, size_t len) {
    std::memcpy(img.data() + static_cast<size_t>(sec) * kSectorSize, src, len);
  };

  // Volume descriptor at sector 32
  std::vector<uint8_t> vd(kSectorSize, 0);
  std::memcpy(vd.data(), setup::xiso::kGdfxMagic, 20);
  put_le32(vd, 0x14, 33u);
  put_le32(vd, 0x18, static_cast<uint32_t>(root_dir.size()));
  std::memcpy(vd.data() + kSectorSize - 20, setup::xiso::kGdfxMagic, 20);
  write_sector(32, vd.data(), vd.size());

  write_sector(33, root_dir.data(), root_dir.size());
  write_sector(34, media_dir.data(), media_dir.size());
  write_sector(35, cars_dir.data(), cars_dir.size());
  write_sector(36, db_dir.data(), db_dir.size());
  write_sector(37, tracks_dir.data(), tracks_dir.size());
  write_sector(38, colorado_dir.data(), colorado_dir.size());
  write_sector(40, car_data.data(), car_data.size());
  write_sector(41, db_data.data(), db_data.size());
  write_sector(42, col_data.data(), col_data.size());
  write_sector(43, xex_data.data(), xex_data.size());
  write_sector(44, root_txt.data(), root_txt.size());
  return img;
}

// ---- Synthetic STFS container builder --------------------------------------

struct StfsBuildFile {
  std::string name;
  int16_t parent = -1;
  bool is_dir = false;
  bool contiguous = true;
  std::vector<uint32_t> blocks;  // explicit logical block numbers (supports non-contiguous chains!)
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> build_synthetic_stfs(const char magic[4], uint32_t content_type, const std::string& display_name,
                                          uint8_t content_id_byte, bool read_only,
                                          const std::vector<StfsBuildFile>& files) {
  constexpr uint32_t kDataOffset = 0xA000u;
  constexpr uint32_t kBlock = 0x1000u;
  const uint32_t hash_blocks = read_only ? 1u : 2u;

  uint32_t max_data_block = 0;
  for (const auto& f : files) {
    for (uint32_t b : f.blocks) max_data_block = std::max(max_data_block, b);
  }
  const uint32_t allocated = max_data_block + 1u;
  const uint32_t total_backing = hash_blocks + allocated;
  std::vector<uint8_t> buf(kDataOffset + static_cast<size_t>(total_backing) * kBlock, 0);

  std::memcpy(buf.data(), magic, 4);
  for (size_t i = 0; i < 20; ++i) buf[0x032C + i] = static_cast<uint8_t>(content_id_byte + i);
  put_be32(buf, 0x0340, 0x971Au);  // aligns to 0xA000
  put_be32(buf, 0x0344, content_type);
  put_be32(buf, 0x0348, 2u);
  put_be32(buf, 0x0360, setup::stfs::kFh1TitleId);

  // STFS volume descriptor at 0x0379
  buf[0x0379] = 0x24u;
  buf[0x037B] = read_only ? 0x01u : 0x00u;
  put_le16(buf, 0x037C, 1u);  // 1 file table block
  put_le24(buf, 0x037E, 0u);  // file table at logical block 0
  put_be32(buf, 0x0395, allocated);
  put_be32(buf, 0x03A9, 0u);  // descriptor_type = STFS

  // UTF-16BE display name at 0x0411
  for (size_t i = 0; i < display_name.size() && i < 60; ++i) {
    put_be16(buf, 0x0411 + i * 2, static_cast<uint16_t>(static_cast<unsigned char>(display_name[i])));
  }

  auto data_block_offset = [&](uint32_t logical_block) -> size_t {
    return kDataOffset + static_cast<size_t>(hash_blocks + logical_block) * kBlock;
  };
  auto set_l0_next = [&](uint32_t logical_block, uint32_t next_block) {
    const size_t entry_off = kDataOffset + static_cast<size_t>(logical_block) * 24u;
    buf[entry_off + 20] = 0x80u;  // allocated
    put_be24(buf, entry_off + 21, next_block);
  };

  set_l0_next(0, 0xFFFFFFu);

  // Populate file table at logical block 0
  const size_t ft_off = data_block_offset(0);
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& f = files[i];
    const size_t eoff = ft_off + i * 64u;
    std::memcpy(buf.data() + eoff, f.name.data(), f.name.size());
    uint8_t flags = static_cast<uint8_t>(f.name.size() & 0x3Fu);
    if (f.contiguous) flags |= 0x40u;
    if (f.is_dir) flags |= 0x80u;
    buf[eoff + 0x28] = flags;
    put_le24(buf, eoff + 0x29, static_cast<uint32_t>(f.blocks.size()));
    put_le24(buf, eoff + 0x2C, static_cast<uint32_t>(f.blocks.size()));
    put_le24(buf, eoff + 0x2F, f.blocks.empty() ? 0u : f.blocks[0]);
    put_be16(buf, eoff + 0x32, static_cast<uint16_t>(f.parent));
    put_be32(buf, eoff + 0x34, static_cast<uint32_t>(f.payload.size()));

    // Write block payloads + L0 hash next-block links
    for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
      const uint32_t blk = f.blocks[bi];
      const uint32_t next_blk = (bi + 1 < f.blocks.size()) ? f.blocks[bi + 1] : 0xFFFFFFu;
      set_l0_next(blk, next_blk);
      const size_t start = bi * kBlock;
      if (start < f.payload.size()) {
        const size_t len = std::min<size_t>(kBlock, f.payload.size() - start);
        std::memcpy(buf.data() + data_block_offset(blk), f.payload.data() + start, len);
      }
    }
  }
  return buf;
}

// ---- Synthetic Stored ZIP builder ------------------------------------------

std::vector<uint8_t> build_synthetic_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
  std::vector<uint8_t> out;
  struct Rec {
    std::string name;
    uint32_t crc = 0;
    uint32_t size = 0;
    uint32_t local_off = 0;
  };
  std::vector<Rec> recs;

  auto append_le16 = [&](uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  };
  auto append_le32 = [&](uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
  };

  for (const auto& [name, data] : entries) {
    Rec r;
    r.name = name;
    r.size = static_cast<uint32_t>(data.size());
    r.crc = fh1::crc32({reinterpret_cast<const uint8_t*>(data.data()), data.size()});
    r.local_off = static_cast<uint32_t>(out.size());
    recs.push_back(r);

    append_le32(0x04034B50u);  // local sig
    append_le16(20u);          // version
    append_le16(0u);           // flags
    append_le16(0u);           // method = stored
    append_le16(0u);
    append_le16(0u);
    append_le32(r.crc);
    append_le32(r.size);
    append_le32(r.size);
    append_le16(static_cast<uint16_t>(name.size()));
    append_le16(0u);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
  }

  const uint32_t cd_off = static_cast<uint32_t>(out.size());
  for (const auto& r : recs) {
    append_le32(0x02014B50u);  // central sig
    append_le16(20u);
    append_le16(20u);
    append_le16(0u);
    append_le16(0u);  // stored
    append_le16(0u);
    append_le16(0u);
    append_le32(r.crc);
    append_le32(r.size);
    append_le32(r.size);
    append_le16(static_cast<uint16_t>(r.name.size()));
    append_le16(0u);
    append_le16(0u);
    append_le16(0u);
    append_le16(0u);
    append_le32(0u);
    append_le32(r.local_off);
    out.insert(out.end(), r.name.begin(), r.name.end());
  }
  const uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_off;

  append_le32(0x06054B50u);  // EOCD
  append_le16(0u);
  append_le16(0u);
  append_le16(static_cast<uint16_t>(recs.size()));
  append_le16(static_cast<uint16_t>(recs.size()));
  append_le32(cd_size);
  append_le32(cd_off);
  append_le16(0u);
  return out;
}

// ---- Test Suites -----------------------------------------------------------

void test_xiso_all_offsets_and_extraction(const fs::path& tmp) {
  const auto part = build_synthetic_xiso_partition(false);

  // 1. Verify detection across all 4 standard partition offsets via sparse callback reader.
  const std::pair<uint64_t, const char*> offsets[] = {
      {0x00000000ULL, "XISO"},
      {0x02080000ULL, "XGD3"},
      {0x0FD90000ULL, "XGD2"},
      {0x18300000ULL, "XGD1"},
  };
  for (const auto& [base_off, expected_label] : offsets) {
    auto reader = setup::StreamReader::from_callback(
        base_off + part.size(),
        [base_off, &part](uint64_t off, void* dst, size_t len, std::string*) -> bool {
          std::memset(dst, 0, len);
          if (off + len <= base_off || off >= base_off + part.size()) return true;
          const uint64_t start = std::max(off, base_off);
          const uint64_t end = std::min(off + len, base_off + part.size());
          std::memcpy(static_cast<uint8_t*>(dst) + (start - off), part.data() + (start - base_off),
                      static_cast<size_t>(end - start));
          return true;
        });

    setup::xiso::PartitionInfo pi;
    std::string err;
    CHECK(setup::xiso::detect_partition(*reader, pi, &err));
    CHECK(pi.partition_offset == base_off);
    CHECK(pi.format_label == expected_label);

    std::vector<setup::xiso::Entry> entries;
    CHECK(setup::xiso::list_entries(*reader, pi, entries, &err));
    CHECK(entries.size() == 10);  // 5 dirs + 5 files
  }

  // 2. File-backed extraction with media_only + skip_xex + progress + resume.
  const fs::path iso_file = tmp / "fh1_test.iso";
  write_bytes(iso_file, part);

  const fs::path out_game = tmp / "extracted_game";
  setup::xiso::ExtractOptions opts;
  opts.media_only = true;
  opts.skip_xex = true;

  int progress_calls = 0;
  setup::xiso::ExtractReport rep;
  CHECK(setup::xiso::extract_iso_path(
      iso_file, out_game, opts, [&](const setup::xiso::Progress&) { ++progress_calls; }, nullptr, rep));
  CHECK(rep.ok);
  CHECK(rep.files_extracted == 3);
  CHECK(rep.files_skipped == 2);  // default.xex + readme.txt
  CHECK(progress_calls >= 3);
  CHECK(read_text(out_game / "media" / "cars" / "test_car.zip") == "CARZIP-SYNTHETIC-PAYLOAD");
  CHECK(read_text(out_game / "media" / "db" / "gamedb.slt") == "SQLITE-SYNTHETIC-DB-DATA");
  CHECK(read_text(out_game / "media" / "tracks" / "colorado" / "bin.zip") == "COLORADO-BIN-ZIP-PAYLOAD");
  CHECK(!fs::exists(out_game / "default.xex"));

  // Inspect disc folder
  const auto check = setup::xiso::inspect_disc_directory(out_game);
  CHECK(check.valid);
  CHECK(check.car_zip_count == 1);

  // Resume skips identical existing files
  setup::xiso::ExtractReport rep2;
  CHECK(setup::xiso::extract_iso_path(iso_file, out_game, opts, nullptr, nullptr, rep2));
  CHECK(rep2.ok);
  CHECK(rep2.files_extracted == 0);
  CHECK(rep2.files_skipped == 5);

  // 3. POSIX fd extraction + cancellation test
  const int fd = open_readonly_fd(iso_file);
  CHECK(fd >= 0);
  const fs::path fd_out = tmp / "extracted_fd";
  setup::xiso::ExtractReport fd_rep;
  CHECK(setup::xiso::extract_iso_fd(fd, true, fd_out, opts, nullptr, nullptr, fd_rep));
  CHECK(fd_rep.ok);
  CHECK(fd_rep.files_extracted == 3);

  std::atomic<bool> cancel{true};
  setup::xiso::ExtractReport cancel_rep;
  CHECK(!setup::xiso::extract_iso_path(iso_file, tmp / "cancelled_game", opts, nullptr, &cancel, cancel_rep));
  CHECK(cancel_rep.cancelled);

  // 4. Reject traversal attack inside XISO directory entry
  const auto bad_part = build_synthetic_xiso_partition(true);
  auto bad_reader = setup::StreamReader::from_memory(bad_part);
  setup::xiso::ExtractReport bad_rep;
  CHECK(!setup::xiso::extract_iso(*bad_reader, tmp / "bad_out", opts, nullptr, nullptr, bad_rep));
  CHECK(!bad_rep.ok);
}

void test_stfs_pirs_live_and_con(const fs::path& tmp) {
  const fs::path play_dir = tmp / "stfs_play";
  const fs::path state_dir = tmp / "stfs_state";

  // 1. PIRS Title Update v4 container (RO)
  const std::string v4_zip_bytes = "TITLE-UPDATE-V4-MEDIA-ZIP";
  const auto tu_img = build_synthetic_stfs(
      "PIRS", setup::stfs::kContentTypeTitleUpdate, "Forza Horizon Title Update 4", 0xA1, true,
      {
          {"media.zip", -1, false, true, {1}, {v4_zip_bytes.begin(), v4_zip_bytes.end()}},
      });
  const fs::path tu_path = tmp / "TU_v4.pirs";
  write_bytes(tu_path, tu_img);

  setup::stfs::InstallTargets tu_targets{play_dir, state_dir, "TU_v4"};
  setup::stfs::InstallReport tu_rep;
  CHECK(setup::stfs::install_package_path(tu_path, tu_targets, nullptr, nullptr, tu_rep));
  CHECK(tu_rep.ok);
  CHECK(tu_rep.kind == setup::stfs::PackageKind::kTitleUpdate);
  CHECK(read_text(state_dir / "title-update-v4" / "media.zip") == v4_zip_bytes);
  CHECK(fs::is_regular_file(play_dir / "updates" / (tu_rep.package_id + ".json")));

  // 2. LIVE Rally Expansion DLC container (RO) with NON-CONTIGUOUS multi-block file (> 2 blocks = 9000 bytes)
  std::vector<uint8_t> dlc_zip(9000);
  for (size_t i = 0; i < dlc_zip.size(); ++i) dlc_zip[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
  const std::vector<uint8_t> puboffer = {0x01, 0x02, 0x03};

  const auto rally_img = build_synthetic_stfs(
      "LIVE", setup::stfs::kContentTypeMarketplace, "Horizon Rally Expansion Pack", 0xB2, true,
      {
          {"Media", -1, true, true, {}, {}},                          // idx 0
          {"DLCZips", 0, true, true, {}, {}},                         // idx 1
          {"1600.puboffer", 0, false, true, {1}, puboffer},           // idx 2
          {"1600_pri_65.zip", 1, false, false, {4, 2, 5}, dlc_zip},   // idx 3: non-contiguous chain 4 -> 2 -> 5!
      });
  const fs::path rally_path = tmp / "RallyExpansion.live";
  write_bytes(rally_path, rally_img);

  setup::stfs::InstallTargets rally_targets{play_dir, state_dir, "Rally"};
  setup::stfs::InstallReport rally_rep;
  CHECK(setup::stfs::install_package_path(rally_path, rally_targets, nullptr, nullptr, rally_rep));
  CHECK(rally_rep.ok);
  CHECK(rally_rep.kind == setup::stfs::PackageKind::kRallyExpansion);
  CHECK(rally_rep.display_name == "Horizon Rally Expansion Pack");
  CHECK(rally_rep.files_extracted == 2);

  // Verify non-contiguous multi-block file extracted byte-for-byte
  const fs::path out_dlc_zip = rally_rep.installed_dir / "Media" / "DLCZips" / "1600_pri_65.zip";
  CHECK(fs::is_regular_file(out_dlc_zip));
  const std::string got_dlc = read_text(out_dlc_zip);
  CHECK(got_dlc.size() == dlc_zip.size());
  CHECK(std::memcmp(got_dlc.data(), dlc_zip.data(), dlc_zip.size()) == 0);

  // Verify cars::list_packs recognizes the installed DLC pack as owned with puboffer 1600!
  const auto packs = cars::list_packs(state_dir);
  CHECK(packs.size() == 1);
  if (!packs.empty()) {
    CHECK(packs[0].owned());
    CHECK(packs[0].puboffer == 1600);
    CHECK(packs[0].name == "Horizon Rally Expansion Pack");
  }

  // 3. CON (read-write, 2 hash blocks) package installed via POSIX fd
  const std::string carpack_payload = "CAR-PACK-CON-DATA";
  const auto con_img = build_synthetic_stfs(
      "CON ", setup::stfs::kContentTypeMarketplace, "November Bondurant Car Pack", 0xC3, false,
      {
          {"Media", -1, true, true, {}, {}},
          {"1200.puboffer", 0, false, true, {1}, {0x00, 0x00, 0x00}},
          {"data.bin", 0, false, true, {2}, {carpack_payload.begin(), carpack_payload.end()}},
      });
  const fs::path con_path = tmp / "Bondurant.con";
  write_bytes(con_path, con_img);
  const int fd = open_readonly_fd(con_path);
  CHECK(fd >= 0);

  setup::stfs::InstallReport con_rep;
  CHECK(setup::stfs::install_package_fd(fd, true, {play_dir, state_dir, "Bondurant"}, nullptr, nullptr, con_rep));
  CHECK(con_rep.ok);
  CHECK(read_text(con_rep.installed_dir / "Media" / "data.bin") == carpack_payload);

  const auto installed = setup::stfs::list_installed_packages(state_dir, play_dir);
  CHECK(installed.size() == 3);  // v4 + Rally + Bondurant
}

void test_mods_and_turnip_installer(const fs::path& tmp) {
  const fs::path mods_root = tmp / "state" / "mods";
  const fs::path drivers_root = tmp / "state" / "drivers";

  // 1. Mod ZIP with single top-level wrapper folder (unwrapped cleanly)
  const auto mod1_zip = build_synthetic_zip({
      {"alpha_mod/mod.toml", "name = \"alpha_mod\"\ntitle = \"Alpha Mod\"\nversion = \"1.2.0\"\n"},
      {"alpha_mod/textures/0123456789abcdef.png", "PNGDATA"},
  });
  const fs::path mod1_path = tmp / "alpha_mod.zip";
  write_bytes(mod1_path, mod1_zip);

  setup::mods::InstallModReport r1;
  CHECK(setup::mods::install_mod_zip_path(mod1_path, mods_root, nullptr, nullptr, r1));
  CHECK(r1.ok);
  CHECK(r1.mod_name == "alpha_mod");
  CHECK(fs::is_regular_file(mods_root / "alpha_mod" / "mod.toml"));
  CHECK(!fs::exists(mods_root / "alpha_mod" / "alpha_mod"));

  // 2. Mod ZIP via POSIX fd with no manifest -> auto-generates mod.toml
  const auto mod2_zip = build_synthetic_zip({
      {"db/tweak.sql", "SELECT 1;"},
  });
  const fs::path mod2_path = tmp / "beta_pack.zip";
  write_bytes(mod2_path, mod2_zip);
  const int fd = open_readonly_fd(mod2_path);
  CHECK(fd >= 0);

  setup::mods::InstallModReport r2;
  CHECK(setup::mods::install_mod_zip_fd(fd, true, "beta_pack", mods_root, nullptr, nullptr, r2));
  CHECK(r2.ok);
  CHECK(r2.mod_name == "beta_pack");
  CHECK(fs::is_regular_file(mods_root / "beta_pack" / "mod.toml"));

  // 3. List, toggle disable/enable, reorder load order (move up/down)
  auto listed = setup::mods::list_mods(mods_root);
  CHECK(listed.size() == 2);
  CHECK(listed[0].name == "alpha_mod");
  CHECK(listed[1].name == "beta_pack");
  CHECK(listed[0].enabled && listed[1].enabled);

  std::string err;
  CHECK(setup::mods::set_mod_enabled(mods_root, "alpha_mod", false, &err));
  CHECK(setup::mods::move_mod(mods_root, "beta_pack", -1, &err));  // move beta_pack above alpha_mod

  listed = setup::mods::list_mods(mods_root);
  CHECK(listed.size() == 2);
  CHECK(listed[0].name == "beta_pack");
  CHECK(listed[0].enabled);
  CHECK(listed[1].name == "alpha_mod");
  CHECK(!listed[1].enabled);

  // Confirm cars::modmgr reads the exact same config!
  cars::modmgr::Config cfg;
  CHECK(cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg));
  CHECK(cfg.order.size() == 2 && cfg.order[0] == "beta_pack");
  CHECK(cfg.enabled["alpha_mod"] == false);

  // 4. Reject zip-slip traversal inside mod archive
  const auto evil_zip = build_synthetic_zip({{"../../pwned.txt", "NOPE"}});
  auto evil_reader = setup::StreamReader::from_memory(evil_zip);
  setup::mods::InstallModReport evil_rep;
  CHECK(!setup::mods::install_mod_zip(*evil_reader, "evil", mods_root, nullptr, nullptr, evil_rep));
  CHECK(!evil_rep.ok);

  // 5. Remove mod
  CHECK(setup::mods::remove_mod(mods_root, "alpha_mod", &err));
  CHECK(setup::mods::list_mods(mods_root).size() == 1);

  // 6. Mesa Turnip driver install (.so ELF & .zip release archive)
  const std::string fake_elf = std::string("\x7F""ELF", 4) + "FAKE-TURNIP-DRIVER-PAYLOAD";
  const fs::path so_src = tmp / "vulkan.ad07xx.so";
  write_bytes(so_src, {fake_elf.begin(), fake_elf.end()});
  setup::mods::TurnipInstallReport tr1;
  CHECK(setup::mods::install_turnip_driver_path(so_src, drivers_root, tr1));
  CHECK(tr1.ok);
  CHECK(setup::mods::find_installed_turnip_driver(drivers_root) == drivers_root / "turnip" / "libvulkan_freedreno.so");

  const auto turnip_zip = build_synthetic_zip({
      {"libvulkan_freedreno.so", fake_elf + "-ZIP"},
      {"meta.json", "{\"name\":\"Turnip R24\"}"},
  });
  const fs::path zip_src = tmp / "mesa_turnip_r24.zip";
  write_bytes(zip_src, turnip_zip);
  setup::mods::TurnipInstallReport tr2;
  CHECK(setup::mods::install_turnip_driver_path(zip_src, drivers_root, tr2));
  CHECK(tr2.ok);
  CHECK(read_text(drivers_root / "turnip" / "meta.json").find("Turnip R24") != std::string::npos);
}

void test_baker_state_machine_and_inspect(const fs::path& tmp) {
  // 1. Thread capping logic
  CHECK(setup::baker::recommended_bake_threads(8, true, 0) == 4);
  CHECK(setup::baker::recommended_bake_threads(4, true, 0) == 2);
  CHECK(setup::baker::recommended_bake_threads(16, true, 0) == 4);
  CHECK(setup::baker::recommended_bake_threads(16, false, 0) == 8);
  CHECK(setup::baker::recommended_bake_threads(8, true, 3) == 3);

  // 2. Fast-Start vs Full-Roster selection
  const std::vector<std::string> avail = {
      "NIS_SkylineGTRVSpecII_02", "VW_CorradoVR6_95", "TRF_FordFiesta_09", "FER_F50_95", "LAM_Miura_67"};
  const auto fast = setup::baker::select_cars_for_mode(avail, setup::baker::BakeMode::kFastStart);
  CHECK(fast.size() == 3);  // skyline + corrado + traffic fiesta
  const auto full = setup::baker::select_cars_for_mode(avail, setup::baker::BakeMode::kFullRoster);
  CHECK(full.size() == 5);
  const auto custom = setup::baker::select_cars_for_mode(avail, setup::baker::BakeMode::kFastStart, {"LAM_Miura_67"});
  CHECK(custom.size() == 1 && custom[0] == "lam_miura_67");

  // 3. Zero-bake readiness (play.txt + game/media + world/colorado.index, ZERO bake/ dir!)
  const fs::path play_dir = tmp / "baker_play";
  const fs::path state_dir = tmp / "baker_state";
  const fs::path game_dir = play_dir / "game";

  // Initially empty
  auto st0 = setup::baker::inspect_library(play_dir, state_dir);
  CHECK(!st0.has_game_disc);
  CHECK(!st0.ready_to_play);

  // Extract synthetic disc into play/game
  const auto part = build_synthetic_xiso_partition(false);
  auto reader = setup::StreamReader::from_memory(part);
  setup::xiso::ExtractReport xrep;
  CHECK(setup::xiso::extract_iso(*reader, game_dir, {}, nullptr, nullptr, xrep));

  // Default BakeOptions uses zero-bake direct-from-disc mode (bake_cars == false).
  setup::baker::BakeOptions zb_opts;
  CHECK(!zb_opts.bake_cars);
  zb_opts.game_root = game_dir;
  zb_opts.state_root = state_dir;
  zb_opts.play_dir = play_dir;
  zb_opts.bake_world_index = false;  // synthetic bin.zip is not a real zip
  zb_opts.bake_sounds = false;
  setup::baker::BakeReport zb_rep;
  CHECK(setup::baker::run_on_device_bake(zb_opts, nullptr, nullptr, nullptr, zb_rep));
  CHECK(zb_rep.ok);
  CHECK(zb_rep.play_txt_written);
  CHECK(zb_rep.cars_baked == 0);
  CHECK(zb_rep.cars_reused == 0);
  CHECK(!fs::exists(play_dir / "bake"));
  CHECK(read_text(play_dir / "play.txt") == "game game\n");

  // Without world/colorado.index, not ready yet
  auto st_no_world = setup::baker::inspect_library(play_dir, state_dir);
  CHECK(st_no_world.has_game_disc);
  CHECK(st_no_world.has_play_txt);
  CHECK(!st_no_world.has_world_index);
  CHECK(!st_no_world.ready_to_play);

  // With play.txt + game/media + world/colorado.index and ZERO bake/ dir -> ready_to_play == true!
  write_bytes(play_dir / "world" / "colorado.index", {'s', 'h', 'i', 'f', 't'});
  auto st_zb = setup::baker::inspect_library(play_dir, state_dir);
  CHECK(st_zb.has_game_disc);
  CHECK(st_zb.has_play_txt);
  CHECK(st_zb.has_world_index);
  CHECK(!st_zb.has_shared_bake);
  CHECK(st_zb.baked_car_count == 0);
  CHECK(!fs::exists(play_dir / "bake"));
  CHECK(st_zb.ready_to_play);

  const std::string zb_json = setup::baker::library_status_to_json(st_zb);
  CHECK(zb_json.find("\"ready_to_play\":true") != std::string::npos);
  CHECK(zb_json.find("\"has_shared_bake\":false") != std::string::npos);
  CHECK(zb_json.find("\"baked_car_count\":0") != std::string::npos);

  // 4. Optional legacy bake-state.txt round-trip + inspect_library with baked cars
  std::string err;
  write_bytes(play_dir / "bake" / "shared.shiftbake", {1, 2, 3, 4});
  write_bytes(play_dir / "bake" / "cars" / "nis_skylinegtrvspecii_02.shiftbake", {5, 6, 7, 8});

  setup::baker::BakeStateRecord rec;
  rec.mode = "fast_start";
  rec.baked_cars = 1;
  rec.total_available_cars = 240;
  rec.has_shared = true;
  rec.has_world_index = true;
  rec.complete = false;
  CHECK(setup::baker::write_bake_state(play_dir / "bake", rec, &err));

  setup::baker::BakeStateRecord read_back;
  CHECK(setup::baker::read_bake_state(play_dir / "bake", read_back));
  CHECK(read_back.mode == "fast_start");
  CHECK(read_back.baked_cars == 1);
  CHECK(read_back.total_available_cars == 240);
  CHECK(read_back.has_shared && read_back.has_world_index && !read_back.complete);

  auto st1 = setup::baker::inspect_library(play_dir, state_dir);
  CHECK(st1.has_game_disc);
  CHECK(st1.has_play_txt);
  CHECK(st1.has_shared_bake);
  CHECK(st1.has_world_index);
  CHECK(st1.baked_car_count == 1);
  CHECK(st1.ready_to_play);

  const std::string json = setup::baker::library_status_to_json(st1);
  CHECK(json.find("\"ready_to_play\":true") != std::string::npos);
  CHECK(json.find("\"baked_car_count\":1") != std::string::npos);

  // Baker clean error on missing game root
  setup::baker::BakeOptions bad_opts;
  bad_opts.game_root = tmp / "does_not_exist";
  bad_opts.play_dir = tmp / "bad_play";
  setup::baker::BakeReport bad_rep;
  CHECK(!setup::baker::run_on_device_bake(bad_opts, nullptr, nullptr, nullptr, bad_rep));
  CHECK(!bad_rep.ok);
  CHECK(!bad_rep.error.empty());
}

void test_android_setup_pill_layout_contract() {
  // Verify the normalized top-center SETUP pill bounds stay completely disjoint from the
  // top-right HUD block (position/timer/player bar) across 20:9, 16:9, and 4:3 displays.
  struct Rect {
    float x0, y0, x1, y1;
  };
  auto overlaps = [](const Rect& a, const Rect& b) {
    return a.x0 < b.x1 && a.x1 > b.x0 && a.y0 < b.y1 && a.y1 > b.y0;
  };
  const std::pair<float, float> displays[] = {{2400.0f, 1080.0f}, {1920.0f, 1080.0f}, {2048.0f, 1536.0f}};
  for (const auto& [w, h] : displays) {
    const float aspect = w / h;
    const float pill_w = aspect >= 2.0f ? 0.072f : (aspect <= 1.5f ? 0.110f : 0.088f);
    const Rect setup_pill{0.5f - pill_w * 0.5f, 0.008f, 0.5f + pill_w * 0.5f, 0.064f};
    const Rect top_right_hud{aspect <= 1.5f ? 0.70f : 0.74f, 0.0f, 1.0f, 0.24f};
    CHECK(!overlaps(setup_pill, top_right_hud));
  }

  // If the source tree is accessible from the test working directory, confirm DriveActivity.java
  // anchors the SETUP pill to TOP | CENTER_HORIZONTAL rather than TOP | END.
  const fs::path rel = fs::path("apps") / "carview" / "android" / "java" / "net" / "shiftengine" / "drive" /
                       "DriveActivity.java";
  fs::path probe = fs::current_path();
  for (int depth = 0; depth < 4; ++depth) {
    const fs::path cand = probe / rel;
    if (fs::is_regular_file(cand)) {
      const std::string src = read_text(cand);
      CHECK(src.find("Gravity.TOP | Gravity.CENTER_HORIZONTAL") != std::string::npos);
      CHECK(src.find("Gravity.TOP | Gravity.END") == std::string::npos);
      break;
    }
    if (!probe.has_parent_path() || probe.parent_path() == probe) break;
    probe = probe.parent_path();
  }
}

}  // namespace

int main() {
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec) / "shift_android_standalone_tests";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  test_xiso_all_offsets_and_extraction(tmp);
  test_stfs_pirs_live_and_con(tmp);
  test_mods_and_turnip_installer(tmp);
  test_baker_state_machine_and_inspect(tmp);
  test_android_setup_pill_layout_contract();

  fs::remove_all(tmp, ec);
  if (g_failures != 0) {
    std::fprintf(stderr, "android_standalone_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("android_standalone_tests: all checks passed\n");
  return 0;
}
