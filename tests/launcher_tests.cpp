#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "launcher_core.h"
#include "setup/stfs.h"
#include "setup/xiso.h"
#include "sha256.h"

namespace {

void put_le16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
  buf[off] = uint8_t(v & 0xFFu);
  buf[off + 1] = uint8_t((v >> 8) & 0xFFu);
}

void put_le24(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = uint8_t(v & 0xFFu);
  buf[off + 1] = uint8_t((v >> 8) & 0xFFu);
  buf[off + 2] = uint8_t((v >> 16) & 0xFFu);
}

void put_le32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = uint8_t(v & 0xFFu);
  buf[off + 1] = uint8_t((v >> 8) & 0xFFu);
  buf[off + 2] = uint8_t((v >> 16) & 0xFFu);
  buf[off + 3] = uint8_t((v >> 24) & 0xFFu);
}

void put_be16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
  buf[off] = uint8_t((v >> 8) & 0xFFu);
  buf[off + 1] = uint8_t(v & 0xFFu);
}

void put_be24(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = uint8_t((v >> 16) & 0xFFu);
  buf[off + 1] = uint8_t((v >> 8) & 0xFFu);
  buf[off + 2] = uint8_t(v & 0xFFu);
}

void put_be32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off] = uint8_t((v >> 24) & 0xFFu);
  buf[off + 1] = uint8_t((v >> 16) & 0xFFu);
  buf[off + 2] = uint8_t((v >> 8) & 0xFFu);
  buf[off + 3] = uint8_t(v & 0xFFu);
}

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
    cursor += uint32_t(14 + nodes[i].name.size());
    cursor = (cursor + 3u) & ~3u;
  }
  std::vector<uint8_t> buf(cursor, 0xFFu);
  for (size_t i = 0; i < nodes.size(); ++i) {
    const uint32_t off = offsets[i];
    put_le16(buf, off + 0, nodes[i].left >= 0 ? uint16_t(offsets[size_t(nodes[i].left)] / 4u) : 0xFFFFu);
    put_le16(buf, off + 2, nodes[i].right >= 0 ? uint16_t(offsets[size_t(nodes[i].right)] / 4u) : 0xFFFFu);
    put_le32(buf, off + 4, nodes[i].sector);
    put_le32(buf, off + 8, nodes[i].size);
    buf[off + 12] = nodes[i].is_dir ? 0x10u : 0x20u;
    buf[off + 13] = uint8_t(nodes[i].name.size());
    std::memcpy(buf.data() + off + 14, nodes[i].name.data(), nodes[i].name.size());
  }
  return buf;
}

std::vector<uint8_t> build_synthetic_xiso_image() {
  using setup::xiso::kSectorSize;
  const std::string car_data = "CARZIP-LAUNCHER-TEST-PAYLOAD";
  const std::string db_data = "SQLITE-LAUNCHER-DB-PAYLOAD";
  const std::string col_data = "COLORADO-BIN-ZIP-PAYLOAD";
  const std::string radio_data = "FSB4-RADIO-PAYLOAD";
  const std::string xex_data = "XEX2-SKIP-ME";

  const auto cars_dir = build_gdfx_dir_table({
      {"test_car.zip", 40, uint32_t(car_data.size()), false, -1, -1},
  });
  const auto db_dir = build_gdfx_dir_table({
      {"gamedb.slt", 41, uint32_t(db_data.size()), false, -1, -1},
  });
  const auto colorado_dir = build_gdfx_dir_table({
      {"bin.zip", 42, uint32_t(col_data.size()), false, -1, -1},
  });
  const auto tracks_dir = build_gdfx_dir_table({
      {"colorado", 38, uint32_t(colorado_dir.size()), true, -1, -1},
  });
  const auto radio_dir = build_gdfx_dir_table({
      {"Radio_Music.fsb", 44, uint32_t(radio_data.size()), false, -1, -1},
  });
  const auto audio_dir = build_gdfx_dir_table({
      {"radio", 45, uint32_t(radio_dir.size()), true, -1, -1},
  });
  const auto media_dir = build_gdfx_dir_table({
      {"db", 36, uint32_t(db_dir.size()), true, 1, 2},
      {"cars", 35, uint32_t(cars_dir.size()), true, 3, -1},
      {"tracks", 37, uint32_t(tracks_dir.size()), true, -1, -1},
      {"audio", 39, uint32_t(audio_dir.size()), true, -1, -1},
  });
  const auto root_dir = build_gdfx_dir_table({
      {"media", 34, uint32_t(media_dir.size()), true, 1, -1},
      {"default.xex", 43, uint32_t(xex_data.size()), false, -1, -1},
  });

  std::vector<uint8_t> img(47u * kSectorSize, 0);
  auto write_sec = [&](uint32_t sec, const void* src, size_t len) {
    std::memcpy(img.data() + size_t(sec) * kSectorSize, src, len);
  };

  std::vector<uint8_t> vd(kSectorSize, 0);
  std::memcpy(vd.data(), setup::xiso::kGdfxMagic, 20);
  put_le32(vd, 0x14, 33u);
  put_le32(vd, 0x18, uint32_t(root_dir.size()));
  std::memcpy(vd.data() + kSectorSize - 20, setup::xiso::kGdfxMagic, 20);
  write_sec(32, vd.data(), vd.size());

  write_sec(33, root_dir.data(), root_dir.size());
  write_sec(34, media_dir.data(), media_dir.size());
  write_sec(35, cars_dir.data(), cars_dir.size());
  write_sec(36, db_dir.data(), db_dir.size());
  write_sec(37, tracks_dir.data(), tracks_dir.size());
  write_sec(38, colorado_dir.data(), colorado_dir.size());
  write_sec(39, audio_dir.data(), audio_dir.size());
  write_sec(40, car_data.data(), car_data.size());
  write_sec(41, db_data.data(), db_data.size());
  write_sec(42, col_data.data(), col_data.size());
  write_sec(43, xex_data.data(), xex_data.size());
  write_sec(44, radio_data.data(), radio_data.size());
  write_sec(45, radio_dir.data(), radio_dir.size());
  return img;
}

struct StfsBuildFile {
  std::string name;
  int16_t parent = -1;
  bool is_dir = false;
  bool contiguous = true;
  std::vector<uint32_t> blocks;
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
  std::vector<uint8_t> buf(kDataOffset + size_t(total_backing) * kBlock, 0);

  std::memcpy(buf.data(), magic, 4);
  for (size_t i = 0; i < 20; ++i) buf[0x032C + i] = uint8_t(content_id_byte + i);
  put_be32(buf, 0x0340, 0x971Au);
  put_be32(buf, 0x0344, content_type);
  put_be32(buf, 0x0348, 2u);
  put_be32(buf, 0x0360, setup::stfs::kFh1TitleId);

  buf[0x0379] = 0x24u;
  buf[0x037B] = read_only ? 0x01u : 0x00u;
  put_le16(buf, 0x037C, 1u);
  put_le24(buf, 0x037E, 0u);
  put_be32(buf, 0x0395, allocated);
  put_be32(buf, 0x03A9, 0u);

  for (size_t i = 0; i < display_name.size() && i < 60; ++i) {
    put_be16(buf, 0x0411 + i * 2, uint16_t(static_cast<unsigned char>(display_name[i])));
  }

  auto data_block_offset = [&](uint32_t logical_block) -> size_t {
    return kDataOffset + size_t(hash_blocks + logical_block) * kBlock;
  };
  auto set_l0_next = [&](uint32_t logical_block, uint32_t next_block) {
    const size_t entry_off = kDataOffset + size_t(logical_block) * 24u;
    buf[entry_off + 20] = 0x80u;
    put_be24(buf, entry_off + 21, next_block);
  };
  set_l0_next(0, 0xFFFFFFu);

  const size_t ft_off = data_block_offset(0);
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& f = files[i];
    const size_t eoff = ft_off + i * 64u;
    std::memcpy(buf.data() + eoff, f.name.data(), f.name.size());
    uint8_t flags = uint8_t(f.name.size() & 0x3Fu);
    if (f.contiguous) flags |= 0x40u;
    if (f.is_dir) flags |= 0x80u;
    buf[eoff + 0x28] = flags;
    put_le24(buf, eoff + 0x29, uint32_t(f.blocks.size()));
    put_le24(buf, eoff + 0x2C, uint32_t(f.blocks.size()));
    put_le24(buf, eoff + 0x2F, f.blocks.empty() ? 0u : f.blocks[0]);
    put_be16(buf, eoff + 0x32, uint16_t(f.parent));
    put_be32(buf, eoff + 0x34, uint32_t(f.payload.size()));

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

void write_binary_file(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::string read_binary_string(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::filesystem::path make_temp_dir(const char* name) {
  const auto p = std::filesystem::temp_directory_path() / name;
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  std::filesystem::create_directories(p, ec);
  return p;
}

std::string http_get_local(uint16_t port, const std::string& path, const std::string& extra_hdr = {}) {
#ifdef _WIN32
  WSADATA d{};
  WSAStartup(MAKEWORD(2, 2), &d);
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(s != INVALID_SOCKET);
#else
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(s >= 0);
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  assert(connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra_hdr + "Connection: close\r\n\r\n";
  send(s, req.data(), int(req.size()), 0);
  std::string resp;
  char buf[2048];
  for (;;) {
    const int n = int(recv(s, buf, sizeof buf, 0));
    if (n <= 0) break;
    resp.append(buf, size_t(n));
  }
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
  return resp;
}

void test_sha256_and_crc32() {
  assert(launcher::sha256_string("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(launcher::sha256_string("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(launcher::crc32_string("123456789") == 0xCBF43926u);
  std::printf("  [ok] sha256 + crc32\n");
}

void test_portable_paths() {
  const auto root = make_temp_dir("shift_launcher_portable_test");
  // Standard mode (no portable.txt)
  const auto std_win = launcher::resolve_launcher_paths(root, false, "windows");
  assert(!std_win.portable);
  assert(std_win.play_dir.filename() == "play");

  const auto std_mac = launcher::resolve_launcher_paths(root, false, "macos");
  assert(!std_mac.portable);
  assert(std_mac.data_root.string().find("Application Support") != std::string::npos);

  const auto std_lin = launcher::resolve_launcher_paths(root, false, "linux");
  assert(!std_lin.portable);

  // Portable mode via portable.txt
  {
    std::ofstream f(root / "portable.txt");
    f << "portable\n";
  }
  const auto port = launcher::resolve_launcher_paths(root, false, "windows");
  assert(port.portable);
  assert(port.data_root == root / "data");
  assert(port.play_dir == root / "data" / "play");
  assert(port.mods_dir == root / "data" / "mods");

  // macOS .app bundle portable.txt next to .app
  const auto mac_bundle = root / "mac_bundle" / "Shift Engine.app" / "Contents" / "MacOS";
  std::filesystem::create_directories(mac_bundle);
  {
    std::ofstream f(root / "mac_bundle" / "portable.txt");
    f << "1\n";
  }
  const auto mac_port = launcher::resolve_launcher_paths(mac_bundle, false, "macos");
  assert(mac_port.portable);
  assert(mac_port.data_root == root / "mac_bundle" / "data");

  // Path length budget warning
  assert(launcher::check_portable_path_length("D:/Shift", 70).empty());
  assert(!launcher::check_portable_path_length(std::string(80, 'a'), 70).empty());

  std::filesystem::remove_all(root);
  std::printf("  [ok] portable & 3-OS path resolution\n");
}

void test_disc_verifier() {
  const auto root = make_temp_dir("shift_launcher_disc_test");
  // ISO hint
  const auto iso = root / "forza.iso";
  {
    std::ofstream f(iso);
    f << "dummy";
  }
  const auto iso_res = launcher::verify_disc_folder(iso);
  assert(!iso_res.valid);
  assert(iso_res.looks_like_iso);

  // Extracted disc structure
  const auto disc = root / "disc";
  std::filesystem::create_directories(disc / "media" / "tracks" / "colorado");
  std::filesystem::create_directories(disc / "media" / "db");
  std::filesystem::create_directories(disc / "media" / "cars");
  {
    std::ofstream(disc / "media" / "tracks" / "colorado" / "bin.zip") << "zip";
    std::ofstream(disc / "media" / "db" / "gamedb.slt") << "dbdata";
    std::ofstream(disc / "media" / "cars" / "nis_skyline.zip") << "car";
  }
  const auto ok = launcher::verify_disc_folder(disc);
  assert(ok.valid);
  assert(ok.car_archive_count == 1);
  assert(ok.db_sha256.size() == 64);
  assert(ok.summary.find("One-Click") == std::string::npos);

  // Passing media/ directly also resolves parent disc root
  const auto via_media = launcher::verify_disc_folder(disc / "media");
  assert(via_media.valid);
  assert(via_media.resolved_root == disc);

  // play.txt roundtrip
  const auto play = root / "play";
  assert(launcher::write_play_config(play, disc));
  launcher::PlayConfig pc;
  assert(launcher::read_play_config(play, pc));
  assert(pc.game_root == disc);

  std::filesystem::remove_all(root);
  std::printf("  [ok] disc verifier & play.txt\n");
}

void test_hardware_advisor() {
  // High-end discrete 120 Hz
  launcher::HardwareInfo hi;
  hi.physical_cores = 8;
  hi.logical_threads = 16;
  hi.refresh_hz = 120;
  hi.gpu = launcher::GpuInfo{"NVIDIA RTX 4080", true, false, 16ull << 30, (1u << 22) | (3u << 12), 0x10DE};
  const auto rec_hi = launcher::recommend_preset(hi);
  assert(rec_hi.preset_id == "performance_120");
  assert(rec_hi.carview_preset == "high");
  assert(rec_hi.warning.empty());

  // Weak 2-core iGPU -> Ultra Low 30
  launcher::HardwareInfo lo;
  lo.physical_cores = 2;
  lo.logical_threads = 4;
  lo.refresh_hz = 60;
  lo.gpu = launcher::GpuInfo{"Intel UHD 620", false, false, 2ull << 30, (1u << 22) | (3u << 12), 0x8086};
  const auto rec_lo = launcher::recommend_preset(lo);
  assert(rec_lo.preset_id == "ultra_low_30");
  assert(rec_lo.carview_preset == "low");

  // Old Vulkan 1.2 driver -> warning
  launcher::HardwareInfo old_vk = hi;
  old_vk.gpu->api_version = (1u << 22) | (2u << 12);
  const auto rec_old = launcher::recommend_preset(old_vk);
  assert(!rec_old.warning.empty());

  // Concise single-line hardware summary with no marketing/slop text
  const std::string desc = launcher::describe_hardware(hi);
  assert(desc.find("NVIDIA RTX 4080") != std::string::npos);
  assert(desc.find("16.0 GB") != std::string::npos);
  assert(desc.find("120 Hz") != std::string::npos);
  assert(desc.find("ADVISOR") == std::string::npos);
  assert(desc.find('\n') == std::string::npos);

  std::printf("  [ok] hardware detection & concise summary\n");
}

void test_steam_vdf() {
  launcher::SteamShortcutEntry e1;
  e1.app_name = "Other Game";
  e1.exe = "\"/usr/bin/other\"";
  e1.start_dir = "\"/usr/bin\"";
  e1.app_id = launcher::steam_shortcut_app_id(e1.exe, e1.app_name);
  assert((e1.app_id & 0x80000000u) != 0);

  const auto bytes1 = launcher::encode_shortcuts_vdf({e1});
  std::vector<launcher::SteamShortcutEntry> parsed;
  assert(launcher::parse_shortcuts_vdf(bytes1, parsed));
  assert(parsed.size() == 1);
  assert(parsed[0].app_name == "Other Game");
  assert(parsed[0].app_id == e1.app_id);

  // Upsert adds Shift Engine without losing Other Game
  launcher::SteamShortcutEntry e2;
  e2.app_name = "Shift Engine";
  e2.exe = "\"/opt/shift/carview\"";
  e2.start_dir = "\"/opt/shift\"";
  e2.launch_options = "--preset high";
  e2.app_id = launcher::steam_shortcut_app_id(e2.exe, e2.app_name);
  bool updated = true;
  const auto bytes2 = launcher::upsert_steam_shortcut_vdf(bytes1, e2, &updated);
  assert(!updated);
  assert(launcher::parse_shortcuts_vdf(bytes2, parsed));
  assert(parsed.size() == 2);
  assert(parsed[1].app_name == "Shift Engine");
  assert(parsed[1].launch_options == "--preset high");

  // Re-upsert replaces in place
  e2.launch_options = "--preset low";
  const auto bytes3 = launcher::upsert_steam_shortcut_vdf(bytes2, e2, &updated);
  assert(updated);
  assert(launcher::parse_shortcuts_vdf(bytes3, parsed));
  assert(parsed.size() == 2);
  assert(parsed[1].launch_options == "--preset low");

  std::printf("  [ok] Steam binary shortcuts.vdf roundtrip & upsert\n");
}

void test_mods_and_dlc() {
  const auto root = make_temp_dir("shift_launcher_mods_test");
  const auto mods_dir = root / "mods";
  const auto state_dir = root / "state";
  std::filesystem::create_directories(mods_dir / "alpha_tex" / "textures");
  std::filesystem::create_directories(mods_dir / "beta_db" / "db");
  {
    std::ofstream(mods_dir / "alpha_tex" / "mod.toml")
        << "title = \"Alpha 4K Textures\"\nversion = \"1.0\"\nauthor = \"Modder\"\nshares_save = true\n"
        << "[options.quality]\ntitle = \"Texture Tier\"\ndefault = \"4K\"\nchoices = [\"2K\", \"4K\"]\n";
    std::ofstream(mods_dir / "alpha_tex" / "textures" / "road.dds") << "tex";

    std::ofstream(mods_dir / "beta_db" / "mod.txt")
        << "title Beta Database Mod\nenabled 1\npriority 5\n";
    std::ofstream(mods_dir / "beta_db" / "db" / "patch.sql") << "SELECT 1;";
  }

  auto mods = launcher::scan_mods(mods_dir);
  assert(mods.size() == 2);
  assert(mods[0].name == "beta_db");  // priority 5 > 0
  assert(mods[1].name == "alpha_tex");
  assert(mods[1].options.size() == 1);
  assert(mods[1].options[0].value == "4K");

  // Database mod triggers user-modded save isolation
  auto iso = launcher::evaluate_save_isolation(mods);
  assert(iso.kind == "modded");
  assert(iso.profile == "user-modded");

  // Toggle database mod off -> back to normal save
  assert(launcher::toggle_mod_enabled(mods, 0));
  iso = launcher::evaluate_save_isolation(mods);
  assert(iso.kind == "own");
  assert(iso.profile == "user");

  // Move alpha_tex above beta_db and verify priority persists
  assert(launcher::move_mod_order(mods, 1, -1));
  const auto reloaded = launcher::scan_mods(mods_dir);
  assert(reloaded[0].name == "alpha_tex");
  assert(reloaded[0].priority > reloaded[1].priority);

  // STFS DLC & v4 + UDLC/XE detection (rejects truncated container, installs real STFS package)
  const auto bad_stfs = root / "corrupt_stfs";
  {
    std::ofstream f(bad_stfs, std::ios::binary);
    f.write("LIVE", 4);
    f << "payload";
  }
  assert(launcher::is_stfs_package_file(bad_stfs));
  std::string imp_msg;
  assert(!launcher::import_content_package(bad_stfs, state_dir, mods_dir, imp_msg));

  const auto rally_img = build_synthetic_stfs(
      "LIVE", setup::stfs::kContentTypeMarketplace, "Horizon Rally Expansion Pack", 0xB2, true,
      {
          {"Media", -1, true, true, {}, {}},
          {"1600.puboffer", 0, false, true, {1}, {0x01, 0x02, 0x03}},
      });
  const auto stfs_src = root / "dlcpak_rally";
  write_binary_file(stfs_src, rally_img);
  assert(launcher::is_stfs_package_file(stfs_src));
  assert(launcher::import_content_package(stfs_src, state_dir, mods_dir, imp_msg));
  std::filesystem::create_directories(state_dir / "title-update-v4");
  std::filesystem::create_directories(mods_dir / "forza_horizon_xe");
  const auto cs = launcher::scan_content_packages(state_dir, mods_dir);
  assert(cs.has_v4);
  assert(cs.has_rally);
  assert(cs.has_xe);
  assert(cs.dlc_installed_count == 1);

  std::filesystem::remove_all(root);
  std::printf("  [ok] mod manager (mod.toml + mod.txt + save isolation) & DLC/v4/XE\n");
}

void test_radio_wheel_updater() {
  // Radio XML merging & PNG 210x100 validation
  const std::string sys_xml =
      "<RadioSystem>\n"
      "  <RadioStation name=\"Radio1\">\n"
      "    <Playlist noRepeat=\"10\">\n"
      "      <MusicTrack name=\"Old\" artist=\"OldA\" fmodname=\"OLD01\" />\n"
      "    </Playlist>\n"
      "  </RadioStation>\n"
      "</RadioSystem>\n";
  const auto replaced = launcher::merge_radio_system_xml(
      sys_xml, "Radio1", {{"Nightcall", "Kavinsky", "SE001_Nightcall"}}, true);
  assert(replaced.find("OLD01") == std::string::npos);
  assert(replaced.find("SE001_Nightcall") != std::string::npos);
  assert(replaced.find("noRepeat=\"0\"") != std::string::npos);

  const auto extended = launcher::merge_radio_system_xml(
      sys_xml, "Radio1", {{"Track2", "Artist2", "SE002_Track2"}}, false);
  assert(extended.find("OLD01") != std::string::npos);
  assert(extended.find("SE002_Track2") != std::string::npos);

  const auto root = make_temp_dir("shift_launcher_misc_test");
  // Synthetic 210x100 PNG header
  const auto good_png = root / "logo_210x100.png";
  {
    uint8_t hdr[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                       0,    0,   0,   13,  'I',  'H',  'D',  'R',
                       0,    0,   0,   210, 0,    0,    0,    100};
    std::ofstream f(good_png, std::ios::binary);
    f.write(reinterpret_cast<const char*>(hdr), 24);
  }
  const auto lv = launcher::validate_station_logo_png(good_png);
  assert(lv.valid && lv.width == 210 && lv.height == 100);

  // Wheel settings roundtrip preserving unrelated carview settings
  const auto sfile = root / "settings.txt";
  {
    std::ofstream f(sfile);
    f << "preset high\nno-traffic\n";
  }
  launcher::WheelBindingConfig wcfg;
  wcfg.separate_devices = true;
  wcfg.steering_degrees = 540;
  wcfg.steer.deadzone_in = 0.08f;
  wcfg.ffb_gain = 0.75f;
  wcfg.ffb_invert = true;
  assert(launcher::save_wheel_settings(sfile, wcfg));
  const auto wload = launcher::load_wheel_settings(sfile);
  assert(wload.separate_devices);
  assert(wload.steering_degrees == 540);
  assert(wload.ffb_invert);
  {
    std::ifstream in(sfile);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    assert(content.find("preset high") != std::string::npos);
    assert(content.find("steer-dz-in 0.08") != std::string::npos);
  }

  // Self-updater JSON selection + SHA-256 verification
  const std::string fake_sha(64, 'a');
  const std::string releases_json =
      "["
      "{\"tag_name\":\"v0.2.0-rc.1\",\"draft\":false,\"prerelease\":true,\"html_url\":\"https://example/rc\","
      " \"assets\":[{\"name\":\"shift-engine-windows.zip\",\"browser_download_url\":\"https://example/rc.zip\","
      " \"size\":1234,\"digest\":\"sha256:" + fake_sha + "\"}]},"
      "{\"tag_name\":\"v0.1.5\",\"draft\":false,\"prerelease\":false,\"html_url\":\"https://example/015\","
      " \"assets\":[{\"name\":\"shift-engine-windows.zip\",\"browser_download_url\":\"https://example/015.zip\","
      " \"size\":5678,\"digest\":\"sha256:" + fake_sha + "\"}]}"
      "]";
  const auto stable_upd = launcher::select_github_update(releases_json, "0.1.0", "stable", "shift-engine-windows.zip");
  assert(stable_upd.has_value());
  assert(stable_upd->version == "0.1.5");
  assert(stable_upd->sha256 == fake_sha);

  const auto prev_upd = launcher::select_github_update(releases_json, "0.1.0", "preview", "shift-engine-windows.zip");
  assert(prev_upd.has_value());
  assert(prev_upd->version == "0.2.0-rc.1");

  const auto up_to_date = launcher::select_github_update(releases_json, "0.2.0", "stable", "shift-engine-windows.zip");
  assert(!up_to_date.has_value());

  std::filesystem::remove_all(root);
  std::printf("  [ok] radio builder, wheel settings & self-updater\n");
}

void test_wifi_sync_server_and_qr() {
  const auto root = make_temp_dir("shift_launcher_wifi_test");
  const auto paths = launcher::resolve_launcher_paths(root, true, "windows");
  std::filesystem::create_directories(paths.play_dir / "bake" / "cars");
  {
    std::ofstream(paths.play_dir / "play.txt") << "game /disc\n";
    std::ofstream(paths.play_dir / "bake" / "cars" / "r34.shiftbake", std::ios::binary) << "0123456789ABCDEF";
  }
  const auto manifest = launcher::build_wifi_sync_manifest(paths, true, false, false, false);
  assert(manifest.pairing_code.size() == 6);
  assert(manifest.token.size() == 16);
  assert(!manifest.groups.empty());
  assert(manifest.groups[0].files.size() == 2);
  assert(manifest.to_json().find("\"schema_version\":1") != std::string::npos);

  const auto qr = launcher::generate_qr_matrix("http://192.168.1.42:47615/abcdef0123456789");
  assert(qr.size == 29 || qr.size == 33);
  assert(qr.at(0, 0) && qr.at(6, 0) && qr.at(0, 6));  // top-left finder pattern

  // Start real loopback HTTP sync server, verify auth, manifest, and HTTP Range resume!
  launcher::WifiSyncServer server;
  assert(server.start(manifest, "127.0.0.1", 0, false));
  const uint16_t port = server.port();
  assert(port > 0);

  const std::string hello = http_get_local(port, "/shift/v1/hello");
  assert(hello.find("200 OK") != std::string::npos);

  const std::string bad_auth = http_get_local(port, "/shift/v1/manifest", "X-Shift-Code: 000000\r\n");
  assert(bad_auth.find("403 Forbidden") != std::string::npos);

  const std::string good_manifest =
      http_get_local(port, "/shift/v1/manifest", "X-Shift-Code: " + manifest.pairing_code + "\r\n");
  assert(good_manifest.find("200 OK") != std::string::npos);
  assert(good_manifest.find("r34.shiftbake") != std::string::npos);

  // Locate index of r34.shiftbake and fetch bytes 10..15 with Range header
  int r34_idx = -1;
  int idx = 0;
  for (const auto& g : manifest.groups) {
    for (const auto& f : g.files) {
      if (f.destination.find("r34.shiftbake") != std::string::npos) r34_idx = idx;
      ++idx;
    }
  }
  assert(r34_idx >= 0);
  const std::string range_resp = http_get_local(
      port, "/shift/v1/file/" + std::to_string(r34_idx),
      "X-Shift-Code: " + manifest.pairing_code + "\r\nRange: bytes=10-\r\n");
  assert(range_resp.find("206 Partial Content") != std::string::npos);
  assert(range_resp.find("ABCDEF") != std::string::npos);

  server.stop();
  std::filesystem::remove_all(root);
  std::printf("  [ok] Wi-Fi Sync LAN HTTP server (auth + manifest + Range 206) & QR matrix\n");
}

void test_preset_mapping_and_cores() {
  assert(launcher::preset_index_for_name("high") == 0);
  assert(launcher::preset_index_for_name("low") == 1);
  assert(launcher::preset_index_for_name("low-spec-60") == 1);
  assert(launcher::preset_index_for_name("smooth-60") == 2);
  assert(launcher::preset_index_for_name("quality-30") == 3);
  assert(launcher::preset_index_for_name("low-power") == 4);

  const auto hw = launcher::probe_hardware();
  assert(hw.physical_cores > 0);
  assert(hw.logical_threads >= hw.physical_cores);
  // On SMT desktop CPUs (e.g. 8C/16T), verify query_physical_cores() does not drop the last core (7/16).
  if (hw.logical_threads >= 4 && (hw.logical_threads % 2) == 0) {
    assert(hw.physical_cores != (hw.logical_threads / 2) - 1);
  }
  std::printf("  [ok] preset_index_for_name (all 5) & physical core count (%dC/%dT)\n", hw.physical_cores,
              hw.logical_threads);
}

void test_graphics_backend_selector() {
  const auto win = launcher::available_backend_choices("windows");
  assert(win.size() == 3);
  assert(std::string(win[0].id) == "auto" && std::string(win[1].id) == "vulkan" && std::string(win[2].id) == "dx12");
  const auto mac = launcher::available_backend_choices("macos");
  assert(mac.size() == 3);
  assert(std::string(mac[0].id) == "auto" && std::string(mac[1].id) == "metal" && std::string(mac[2].id) == "vulkan");
  const auto lin = launcher::available_backend_choices("linux");
  assert(lin.size() == 2);
  assert(std::string(lin[0].id) == "auto" && std::string(lin[1].id) == "vulkan");

  const auto root = make_temp_dir("shift_launcher_backend_test");
  const auto sf = root / "settings.txt";
  assert(launcher::load_graphics_backend_setting(sf) == "auto");
  assert(launcher::save_graphics_backend_setting(sf, "dx12"));
  assert(launcher::load_graphics_backend_setting(sf) == "dx12");
  assert(launcher::save_graphics_backend_setting(sf, "metal"));
  assert(launcher::load_graphics_backend_setting(sf) == "metal");
  assert(launcher::save_graphics_backend_setting(sf, "auto"));
  assert(launcher::load_graphics_backend_setting(sf) == "auto");
  std::filesystem::remove_all(root);
  std::printf("  [ok] graphics backend selector (Windows/macOS/Linux + settings.txt round-trip)\n");
}

void test_iso_probe_and_setup_extraction() {
  const auto root = make_temp_dir("shift_launcher_iso_test");
  const auto play_dir = root / "play";
  const auto iso_path = root / "Forza_Horizon.iso";

  assert(launcher::is_iso_image_path(iso_path));
  assert(launcher::is_iso_image_path(root / "DISC.ISO"));
  assert(launcher::is_iso_image_path(root / "game.xiso"));
  assert(!launcher::is_iso_image_path(root / "disc_folder"));

  // Write valid synthetic GDFX/XISO image
  const auto iso_bytes = build_synthetic_xiso_image();
  write_binary_file(iso_path, iso_bytes);

  const auto probed = launcher::verify_disc_folder(iso_path, play_dir);
  assert(probed.valid);
  assert(probed.looks_like_iso);
  assert(probed.iso_format == "XISO");
  assert(probed.iso_file_count == 4);  // 4 files under media/, skipping default.xex
  assert(probed.car_archive_count == 1);
  assert(probed.iso_extract_bytes > 0);
  assert(probed.required_space_bytes > probed.iso_extract_bytes);
  assert(probed.items.size() == 6);
  assert(probed.summary.find("XISO ISO verified") != std::string::npos);

  // Direct extract_disc_iso with progress & cancellation check
  int progress_events = 0;
  const auto direct_out = root / "direct_game";
  const auto xres = launcher::extract_disc_iso(
      iso_path, direct_out, [&](const launcher::IsoExtractProgress&) { ++progress_events; });
  assert(xres.ok);
  assert(!xres.cancelled);
  assert(xres.format_label == "XISO");
  assert(xres.files_extracted == 4);
  assert(progress_events >= 4);
  assert(read_binary_string(direct_out / "media" / "cars" / "test_car.zip") == "CARZIP-LAUNCHER-TEST-PAYLOAD");
  assert(read_binary_string(direct_out / "media" / "db" / "gamedb.slt") == "SQLITE-LAUNCHER-DB-PAYLOAD");
  assert(read_binary_string(direct_out / "media" / "tracks" / "colorado" / "bin.zip") == "COLORADO-BIN-ZIP-PAYLOAD");
  assert(!std::filesystem::exists(direct_out / "default.xex"));

  std::atomic<bool> cancel_flag{true};
  const auto cancel_res = launcher::extract_disc_iso(iso_path, root / "cancelled_game", {}, &cancel_flag);
  assert(!cancel_res.ok);
  assert(cancel_res.cancelled);

  // Async SetupRunner extraction into <play_dir>/game/media/ + play.txt persistence
  launcher::LauncherPaths paths = launcher::resolve_launcher_paths(root, true, "windows");
  launcher::SetupOptions sopts;
  sopts.game_root = iso_path;
  sopts.state_root = paths.state_dir;
  sopts.play_dir = play_dir;
  sopts.build_index = false;
  sopts.skip_bake = true;

  launcher::SetupRunner runner;
  assert(runner.start(paths, sopts));
  launcher::SetupProgress prog;
  for (int i = 0; i < 400; ++i) {
    prog = runner.snapshot();
    if (prog.finished) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(prog.finished);
  assert(prog.success);
  assert(prog.stage_count == 5);
  assert(prog.fraction == 1.0f);
  bool saw_iso_log = false;
  for (const auto& line : prog.log_lines) {
    if (line.find("[ISO]") != std::string::npos) saw_iso_log = true;
  }
  assert(saw_iso_log);
  assert(read_binary_string(play_dir / "game" / "media" / "db" / "gamedb.slt") == "SQLITE-LAUNCHER-DB-PAYLOAD");
  launcher::PlayConfig pc;
  assert(launcher::read_play_config(play_dir, pc));
  assert(pc.game_root == play_dir / "game");

  std::filesystem::remove_all(root);
  std::printf("  [ok] XISO/GDFX probe, direct extract, cancel & SetupRunner worker\n");
}

void test_stfs_tu_v4_and_dlc_install() {
  const auto root = make_temp_dir("shift_launcher_stfs_test");
  const auto state_dir = root / "state";
  const auto mods_dir = root / "mods";
  const auto play_dir = root / "play";

  // 1. PIRS Title Update v4 via import_content_package
  const std::string tu_payload = "TITLE-UPDATE-V4-SYNTHETIC-ZIP";
  const auto tu_img = build_synthetic_stfs(
      "PIRS", setup::stfs::kContentTypeTitleUpdate, "Forza Horizon Title Update 4", 0xA1, true,
      {
          {"media.zip", -1, false, true, {1}, {tu_payload.begin(), tu_payload.end()}},
      });
  const auto tu_path = root / "TU_v4.pirs";
  write_binary_file(tu_path, tu_img);

  std::string msg;
  assert(launcher::import_content_package(tu_path, state_dir, mods_dir, msg, play_dir));
  assert(read_binary_string(state_dir / "title-update-v4" / "media.zip") == tu_payload);

  // 2. LIVE Rally Expansion (non-contiguous 3-block file), LIVE 1000 Club & CON Car Pack via ContentInstallRunner
  std::vector<uint8_t> rally_zip(9000);
  for (size_t i = 0; i < rally_zip.size(); ++i) rally_zip[i] = uint8_t((i * 29u + 7u) & 0xFFu);

  const auto rally_img = build_synthetic_stfs(
      "LIVE", setup::stfs::kContentTypeMarketplace, "Horizon Rally Expansion Pack", 0xB2, true,
      {
          {"Media", -1, true, true, {}, {}},
          {"DLCZips", 0, true, true, {}, {}},
          {"1600.puboffer", 0, false, true, {1}, {0x01, 0x02, 0x03}},
          {"1600_pri_65.zip", 1, false, false, {4, 2, 5}, rally_zip},
      });
  const auto club_img = build_synthetic_stfs(
      "LIVE", setup::stfs::kContentTypeMarketplace, "1000 Club Expansion Pack", 0xC4, true,
      {
          {"Media", -1, true, true, {}, {}},
          {"ClubChallenges.xml", 0, false, true, {1}, {'<', 'c', '/', '>'}},
      });
  const std::string car_pack_data = "NOVEMBER-BONDURANT-DLC";
  const auto con_img = build_synthetic_stfs(
      "CON ", setup::stfs::kContentTypeMarketplace, "November Bondurant Car Pack", 0xD5, false,
      {
          {"Media", -1, true, true, {}, {}},
          {"1200.puboffer", 0, false, true, {1}, {0x00, 0x00, 0x00}},
          {"pack.bin", 0, false, true, {2}, {car_pack_data.begin(), car_pack_data.end()}},
      });

  const auto rally_path = root / "RallyExpansion.live";
  const auto club_path = root / "Club1000.live";
  const auto con_path = root / "Bondurant.con";
  write_binary_file(rally_path, rally_img);
  write_binary_file(club_path, club_img);
  write_binary_file(con_path, con_img);

  launcher::ContentInstallRunner installer;
  assert(installer.start({rally_path, club_path, con_path}, state_dir, mods_dir, play_dir));
  launcher::ContentInstallProgress cprog;
  for (int i = 0; i < 400; ++i) {
    cprog = installer.snapshot();
    if (cprog.finished) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(cprog.finished);
  assert(cprog.success);
  assert(cprog.packages_done == 3);
  assert(cprog.packages_total == 3);

  const auto status = launcher::scan_content_packages(state_dir, mods_dir, play_dir);
  assert(status.has_v4);
  assert(status.has_rally);
  assert(status.has_1000_club);
  assert(status.dlc_installed_count == 3);

  // Confirm non-contiguous multi-block file extracted byte-for-byte
  bool verified_rally_bytes = false;
  for (const auto& pkg : status.packages) {
    if (pkg.kind == launcher::ContentKind::kRallyExpansion && pkg.installed) {
      const auto got = read_binary_string(pkg.path / "Media" / "DLCZips" / "1600_pri_65.zip");
      assert(got.size() == rally_zip.size());
      assert(std::memcmp(got.data(), rally_zip.data(), rally_zip.size()) == 0);
      verified_rally_bytes = true;
    }
  }
  assert(verified_rally_bytes);

  std::filesystem::remove_all(root);
  std::printf("  [ok] STFS Title Update v4 + Rally/1000 Club/DLC install & scan\n");
}

}  // namespace

int main() {
  std::printf("launcher_tests:\n");
  test_sha256_and_crc32();
  test_portable_paths();
  test_disc_verifier();
  test_iso_probe_and_setup_extraction();
  test_hardware_advisor();
  test_preset_mapping_and_cores();
  test_graphics_backend_selector();
  test_steam_vdf();
  test_mods_and_dlc();
  test_stfs_tu_v4_and_dlc_install();
  test_radio_wheel_updater();
  test_wifi_sync_server_and_qr();
  std::printf("launcher_tests: all passed.\n");
  return 0;
}
