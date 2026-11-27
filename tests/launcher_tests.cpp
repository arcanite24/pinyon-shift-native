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

#include "drive/save.h"
#include "fh1data/archive.h"
#include "garage/garage.h"
#include "launcher_core.h"
#include "profile/drive_save.h"
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
#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
  send(s, req.data(), int(req.size()), kSendFlags);
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

  // play.txt roundtrip & zero-bake readiness (ready without any bake/cars/*.shiftbake)
  const auto play = root / "play";
  assert(launcher::write_play_config(play, disc));
  launcher::PlayConfig pc;
  assert(launcher::read_play_config(play, pc));
  assert(pc.game_root == disc);
  assert(pc.game_root_valid);
  assert(pc.disc_car_count == 1);
  assert(pc.baked_car_count == 0);
  assert(pc.is_ready());

  // play.txt pointing to an invalid/missing disc is NOT ready
  const auto bad_play = root / "bad_play";
  assert(launcher::write_play_config(bad_play, root / "missing_disc"));
  launcher::PlayConfig bad_pc;
  assert(launcher::read_play_config(bad_play, bad_pc));
  assert(!bad_pc.game_root_valid);
  assert(!bad_pc.is_ready());

  std::filesystem::remove_all(root);
  std::printf("  [ok] disc verifier & zero-bake play.txt readiness\n");
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

  // Old Vulkan 1.2 driver on desktop NVIDIA -> warning
  launcher::HardwareInfo old_vk = hi;
  old_vk.gpu->api_version = (1u << 22) | (2u << 12);
  const auto rec_old = launcher::recommend_preset(old_vk);
  assert(!rec_old.warning.empty());

  // Apple M4 Pro (24 GB unified, 120 Hz, 12C/12T, 0x106B, discrete=false, Vulkan 1.2 via MoltenVK):
  // must NEVER warn "Driver update recommended" and must get Ultra / high-tier preset!
  launcher::HardwareInfo m4pro;
  m4pro.physical_cores = 12;
  m4pro.logical_threads = 12;
  m4pro.refresh_hz = 120;
  m4pro.gpu = launcher::GpuInfo{"Apple M4 Pro", false, false, 24ull << 30, (1u << 22) | (2u << 12), 0x106B, true};
  const auto rec_m4 = launcher::recommend_preset(m4pro);
  assert(rec_m4.warning.empty());
  assert(rec_m4.carview_preset == "ultra");
  assert(rec_m4.preset_id != "low_spec_60");
  assert(rec_m4.label == "Ultra 120");
  assert(rec_m4.driver_url == "https://support.apple.com/macos");
  assert(launcher::vendor_driver_url(0x106B) == "https://support.apple.com/macos");

  // Apple Silicon 16 GB 60 Hz -> High / Quality 60, no warning
  launcher::HardwareInfo m2_16 = m4pro;
  m2_16.refresh_hz = 60;
  m2_16.gpu->name = "Apple M2 Pro";
  m2_16.gpu->vram_bytes = 16ull << 30;
  const auto rec_m2 = launcher::recommend_preset(m2_16);
  assert(rec_m2.warning.empty());
  assert(rec_m2.carview_preset == "high");
  assert(rec_m2.label == "Quality 60");

  // Apple Silicon 8 GB -> Smooth 60, no warning
  launcher::HardwareInfo m1_8 = m2_16;
  m1_8.gpu->name = "Apple M1";
  m1_8.gpu->vram_bytes = 8ull << 30;
  const auto rec_m1 = launcher::recommend_preset(m1_8);
  assert(rec_m1.warning.empty());
  assert(rec_m1.carview_preset == "smooth-60");

  // Software renderer (llvmpipe) -> warning even if portability set
  launcher::HardwareInfo sw = hi;
  sw.gpu = launcher::GpuInfo{"llvmpipe (LLVM 17)", false, true, 4ull << 30, (1u << 22) | (3u << 12), 0x10005, false};
  assert(!launcher::recommend_preset(sw).warning.empty());

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

  // Versioned package.py release assets (shift-engine-<platform>-<ver>.zip / .tar.gz / .dmg + .sha256)
  assert(launcher::matches_release_asset("shift-engine-windows-0.2.0.zip", "windows"));
  assert(launcher::matches_release_asset("shift-engine-windows-v0.2.0.zip", "shift-engine-windows-x64.zip"));
  assert(launcher::matches_release_asset("shift-engine-linux-0.2.0.tar.gz", "linux"));
  assert(launcher::matches_release_asset("shift-engine-macos-0.2.0.dmg", "macos"));
  assert(!launcher::matches_release_asset("shift-engine-windows-0.2.0.zip.sha256", "windows"));
  assert(!launcher::matches_release_asset("shift-engine-linux-0.2.0.zip", "windows"));

  const std::string ver_sha(64, 'b');
  const std::string versioned_json =
      "["
      "{\"tag_name\":\"v0.2.1\",\"draft\":false,\"prerelease\":false,\"html_url\":\"https://example/021\","
      " \"assets\":["
      "  {\"name\":\"shift-engine-windows-0.2.1.zip.sha256\",\"browser_download_url\":\"https://example/win.sha256\",\"size\":96},"
      "  {\"name\":\"shift-engine-windows-0.2.1.zip\",\"browser_download_url\":\"https://example/win.zip\",\"size\":9999},"
      "  {\"name\":\"shift-engine-linux-0.2.1.tar.gz\",\"browser_download_url\":\"https://example/lin.tar.gz\",\"size\":8888,"
      "   \"digest\":\"sha256:" + ver_sha + "\"}"
      "]}"
      "]";
  const auto win_ver = launcher::select_github_update(versioned_json, "0.1.0", "stable", "windows");
  assert(win_ver.has_value());
  assert(win_ver->asset_name == "shift-engine-windows-0.2.1.zip");
  assert(win_ver->sha256.empty());
  assert(win_ver->sha256_url == "https://example/win.sha256");

  const auto lin_ver = launcher::select_github_update(versioned_json, "0.1.0", "stable", "linux");
  assert(lin_ver.has_value());
  assert(lin_ver->asset_name == "shift-engine-linux-0.2.1.tar.gz");
  assert(lin_ver->sha256 == ver_sha);

  // Checksum file parsing (bare, sha256: prefixed, and multi-line SHA256SUMS)
  assert(launcher::parse_sha256_checksum_text(ver_sha + "  shift-engine-windows-0.2.1.zip\n",
                                              "shift-engine-windows-0.2.1.zip") == ver_sha);
  const std::string sums_txt = fake_sha + "  shift-engine-linux-0.2.1.zip\n" +
                               ver_sha + " *shift-engine-windows-0.2.1.zip\n";
  assert(launcher::parse_sha256_checksum_text(sums_txt, "shift-engine-windows-0.2.1.zip") == ver_sha);

  // plan_staged_update unwraps single top-level release directory & preserves subpaths
  const auto unpacked = root / "unpacked";
  const auto install_dir = root / "install";
  std::filesystem::create_directories(unpacked / "shift-engine-windows-0.2.1" / "docs");
  {
    std::ofstream f(unpacked / "shift-engine-windows-0.2.1" / "shift-launcher.exe", std::ios::binary);
    f << "NEWBIN";
  }
  {
    std::ofstream f(unpacked / "shift-engine-windows-0.2.1" / "docs" / "README.md", std::ios::binary);
    f << "README";
  }
  const auto plan = launcher::plan_staged_update(unpacked, install_dir);
  assert(plan.moves.size() == 2);
  bool found_launcher = false;
  bool found_readme = false;
  for (const auto& [src, dst] : plan.moves) {
    if (dst == install_dir / "shift-launcher.exe") found_launcher = true;
    if (dst == install_dir / "docs" / "README.md") found_readme = true;
  }
  assert(found_launcher && found_readme);

  // prepare_staged_update_from_archive safely blocks missing & mismatched SHA-256
  const auto fake_zip = root / "shift-engine-windows-0.2.1.zip";
  {
    std::ofstream f(fake_zip, std::ios::binary);
    f << "not-matching-digest";
  }
  const auto no_sha = launcher::prepare_staged_update_from_archive(fake_zip, "", install_dir);
  assert(!no_sha.ok && !no_sha.verified);
  const auto bad_sha = launcher::prepare_staged_update_from_archive(fake_zip, ver_sha, install_dir);
  assert(!bad_sha.ok && !bad_sha.verified);
  assert(bad_sha.status.find("mismatch") != std::string::npos);

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
  assert(launcher::preset_index_for_name("ultra") == 0);
  assert(launcher::preset_index_for_name("ULTRA") == 0);
  assert(launcher::preset_index_for_name("high") == 1);
  assert(launcher::preset_index_for_name("low") == 2);
  assert(launcher::preset_index_for_name("low-spec-60") == 2);
  assert(launcher::preset_index_for_name("smooth-60") == 3);
  assert(launcher::preset_index_for_name("quality-30") == 4);
  assert(launcher::preset_index_for_name("low-power") == 5);
  assert(std::string(launcher::preset_name_for_index(0)) == "ultra");
  assert(std::string(launcher::preset_name_for_index(1)) == "high");
  assert(std::string(launcher::preset_name_for_index(2)) == "low");
  assert(std::string(launcher::preset_name_for_index(3)) == "smooth-60");
  assert(std::string(launcher::preset_name_for_index(4)) == "quality-30");
  assert(std::string(launcher::preset_name_for_index(5)) == "low-power");

  // Preset persistence round-trip (including "ultra") preserving unrelated settings.txt lines.
  const auto root = make_temp_dir("shift_launcher_preset_test");
  const auto sf = root / "settings.txt";
  {
    std::ofstream f(sf);
    f << "no-traffic\ngraphics_backend vulkan\n";
  }
  assert(launcher::load_graphics_preset_setting(sf) == "high");
  assert(launcher::save_graphics_preset_setting(sf, "ultra"));
  assert(launcher::load_graphics_preset_setting(sf) == "ultra");
  assert(launcher::save_graphics_preset_setting(sf, "low-power"));
  assert(launcher::load_graphics_preset_setting(sf) == "low-power");
  assert(launcher::save_graphics_preset_setting(sf, "ultra"));
  {
    const std::string txt = read_binary_string(sf);
    assert(txt.find("preset ultra") != std::string::npos);
    assert(txt.find("no-traffic") != std::string::npos);
    assert(txt.find("graphics_backend vulkan") != std::string::npos);
  }
  std::filesystem::remove_all(root);

  const auto hw = launcher::probe_hardware();
  assert(hw.physical_cores > 0);
  assert(hw.logical_threads >= hw.physical_cores);
  // On SMT desktop CPUs (e.g. 8C/16T), verify query_physical_cores() does not drop the last core (7/16).
  if (hw.logical_threads >= 4 && (hw.logical_threads % 2) == 0) {
    assert(hw.physical_cores != (hw.logical_threads / 2) - 1);
  }
  std::printf("  [ok] preset_index_for_name (all 6 incl. ultra), preset settings & physical core count (%dC/%dT)\n",
              hw.physical_cores, hw.logical_threads);
}

void test_launcher_sounds_and_posix_quoting() {
  // toggle_sounds persistence round-trip preserving unrelated settings.txt lines
  const auto root = make_temp_dir("shift_launcher_sounds_test");
  const auto sf = root / "settings.txt";
  assert(launcher::load_launcher_sounds_setting(sf) == true);  // default on
  {
    std::ofstream f(sf);
    f << "preset ultra\nno-traffic\n";
  }
  assert(launcher::save_launcher_sounds_setting(sf, false));
  assert(launcher::load_launcher_sounds_setting(sf) == false);
  {
    const std::string txt = read_binary_string(sf);
    assert(txt.find("# launcher-sounds 0") != std::string::npos);
    assert(txt.find("preset ultra") != std::string::npos);
    assert(txt.find("no-traffic") != std::string::npos);
  }
  assert(launcher::save_launcher_sounds_setting(sf, true));
  assert(launcher::load_launcher_sounds_setting(sf) == true);
  assert(launcher::load_graphics_preset_setting(sf) == "ultra");
  std::filesystem::remove_all(root);

  // shell_quote_posix edge cases: empty, spaces, single quotes, $, backticks, subshells, semicolons
  assert(launcher::shell_quote_posix("") == "''");
  assert(launcher::shell_quote_posix("plain") == "'plain'");
  assert(launcher::shell_quote_posix("/path with spaces/Forza Horizon") == "'/path with spaces/Forza Horizon'");
  assert(launcher::shell_quote_posix("O'Reilly's $HOME") == "'O'\\''Reilly'\\''s $HOME'");
  assert(launcher::shell_quote_posix("`whoami`; $(rm -rf /)") == "'`whoami`; $(rm -rf /)'");
  assert(launcher::shell_quote_posix("a\"b\\c|d&e") == "'a\"b\\c|d&e'");
  assert(launcher::shell_quote_posix("'") == "''\\'''");

  // split_command_args quoting edge cases
  const auto toks = launcher::split_command_args("--hour 18 --car 'NIS Skyline' --save \"/tmp/my save.bin\"");
  assert(toks.size() == 6);
  assert(toks[0] == "--hour" && toks[1] == "18");
  assert(toks[2] == "--car" && toks[3] == "NIS Skyline");
  assert(toks[4] == "--save" && toks[5] == "/tmp/my save.bin");

  std::printf("  [ok] toggle_sounds persistence, shell_quote_posix & split_command_args edge cases\n");
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
  assert(prog.stage_count == 3);
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
  assert(pc.game_root_valid);
  assert(pc.disc_car_count == 1);
  assert(pc.baked_car_count == 0);
  assert(pc.is_ready());
  assert(!std::filesystem::exists(play_dir / "bake"));

  std::filesystem::remove_all(root);
  std::printf("  [ok] XISO/GDFX probe, direct extract, cancel & SetupRunner worker\n");
}

void test_zerobake_setup_runner() {
  const auto root = make_temp_dir("shift_launcher_zerobake_test");
  const auto disc = root / "disc";
  const auto play_dir = root / "play";
  std::filesystem::create_directories(disc / "media" / "tracks" / "colorado");
  std::filesystem::create_directories(disc / "media" / "db");
  std::filesystem::create_directories(disc / "media" / "cars");
  {
    std::ofstream(disc / "media" / "tracks" / "colorado" / "bin.zip") << "zip";
    std::ofstream(disc / "media" / "db" / "gamedb.slt") << "dbdata";
    std::ofstream(disc / "media" / "cars" / "VIP_Viper_13.zip") << "car";
    std::ofstream(disc / "media" / "cars" / "NIS_SkylineGTRVSpecII_02.zip") << "car2";
  }

  // Zero-bake SetupRunner must succeed with NO carbake/soundbake binaries and create NO bake/ or sound/ dirs.
  launcher::LauncherPaths paths = launcher::resolve_launcher_paths(root, true, "windows");
  paths.carbake_bin.clear();
  paths.soundbake_bin.clear();
  paths.carview_bin.clear();

  launcher::SetupOptions sopts;
  sopts.game_root = disc;
  sopts.state_root = paths.state_dir;
  sopts.play_dir = play_dir;
  sopts.build_index = false;
  sopts.skip_bake = false;

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
  assert(prog.stage_count == 2);
  assert(prog.fraction == 1.0f);
  assert(prog.stage_label == "Game Ready");
  assert(!std::filesystem::exists(play_dir / "bake"));
  assert(!std::filesystem::exists(play_dir / "sound"));

  launcher::PlayConfig pc;
  assert(launcher::read_play_config(play_dir, pc));
  assert(pc.game_root == disc);
  assert(pc.game_root_valid);
  assert(pc.disc_car_count == 2);
  assert(pc.baked_car_count == 0);
  assert(pc.is_ready());

  std::filesystem::remove_all(root);
  std::printf("  [ok] zero-bake SetupRunner (no carbake/soundbake, instant Game Ready)\n");
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

void write_stored_zip_local(const std::filesystem::path& zip,
                            const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<uint8_t> out, cd;
  auto le16 = [](std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x & 0xFF));
    v.push_back(uint8_t(x >> 8));
  };
  auto le32 = [&](std::vector<uint8_t>& v, uint32_t x) {
    le16(v, uint16_t(x & 0xFFFF));
    le16(v, uint16_t(x >> 16));
  };
  for (const auto& [name, data] : files) {
    const uint32_t crc = fh1::crc32(data);
    const uint32_t off = uint32_t(out.size());
    le32(out, 0x04034B50); le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
    le32(out, crc); le32(out, uint32_t(data.size())); le32(out, uint32_t(data.size()));
    le16(out, uint16_t(name.size())); le16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());

    le32(cd, 0x02014B50); le16(cd, 20); le16(cd, 20); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0);
    le32(cd, crc); le32(cd, uint32_t(data.size())); le32(cd, uint32_t(data.size()));
    le16(cd, uint16_t(name.size())); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, off);
    cd.insert(cd.end(), name.begin(), name.end());
  }
  const uint32_t cd_off = uint32_t(out.size());
  out.insert(out.end(), cd.begin(), cd.end());
  le32(out, 0x06054B50); le16(out, 0); le16(out, 0);
  le16(out, uint16_t(files.size())); le16(out, uint16_t(files.size()));
  le32(out, uint32_t(cd.size())); le32(out, cd_off); le16(out, 0);
  write_binary_file(zip, out);
}

void test_bidirectional_settings_sync() {
  const auto root = make_temp_dir("shift_launcher_sync_settings_test");
  const auto sf = root / "settings.txt";
  {
    std::ofstream f(sf, std::ios::binary | std::ios::trunc);
    f << "# user comment\nno-traffic\ndifficulty hard\n";
  }

  assert(launcher::save_graphics_preset_setting(sf, "ultra"));
  assert(launcher::save_resolution_setting(sf, 2));  // 1920x1080
  assert(launcher::save_fullscreen_setting(sf, true));
  assert(launcher::save_graphics_backend_setting(sf, "metal"));
  assert(launcher::save_vsync_setting(sf, "mailbox"));
  assert(launcher::save_fps_cap_setting(sf, "120"));
  assert(launcher::save_profile_setting(sf, "weekend"));

  assert(launcher::load_graphics_preset_setting(sf) == "ultra");
  assert(launcher::load_resolution_setting(sf) == 2);
  assert(launcher::load_fullscreen_setting(sf) == true);
  assert(launcher::load_graphics_backend_setting(sf) == "metal");
  assert(launcher::load_vsync_setting(sf) == "mailbox");
  assert(launcher::load_fps_cap_setting(sf) == "120");
  assert(launcher::load_profile_setting(sf) == "weekend");

  // Unrelated lines must be preserved untouched.
  {
    const std::string txt = read_binary_string(sf);
    assert(txt.find("# user comment") != std::string::npos);
    assert(txt.find("no-traffic") != std::string::npos);
    assert(txt.find("difficulty hard") != std::string::npos);
    assert(txt.find("size 1920x1080") != std::string::npos);
    assert(txt.find("fullscreen") != std::string::npos);
    assert(txt.find("vsync mailbox") != std::string::npos);
    assert(txt.find("fps-cap 120") != std::string::npos);
    assert(txt.find("profile weekend") != std::string::npos);
  }

  // Switching resolution to Auto removes `size`; switching fullscreen off writes `no-fullscreen`.
  assert(launcher::save_resolution_setting(sf, 0));
  assert(launcher::save_fullscreen_setting(sf, false));
  assert(launcher::save_vsync_setting(sf, "immediate"));
  assert(launcher::save_fps_cap_setting(sf, "display"));
  assert(launcher::load_resolution_setting(sf) == 0);
  assert(launcher::load_fullscreen_setting(sf) == false);
  assert(launcher::load_vsync_setting(sf) == "immediate");
  assert(launcher::load_fps_cap_setting(sf) == "display");
  {
    const std::string txt = read_binary_string(sf);
    assert(txt.find("size ") == std::string::npos);
    assert(txt.find("no-fullscreen") != std::string::npos);
    assert(txt.find("no-traffic") != std::string::npos);
  }

  std::filesystem::remove_all(root);
  std::printf("  [ok] bidirectional settings.txt round-trip (preset, size, fullscreen, backend, vsync, fps-cap, profile)\n");
}

void test_save_management_and_pinyon_import() {
  const auto root = make_temp_dir("shift_launcher_saves_test");
  const auto paths = launcher::resolve_launcher_paths(root, true, "windows");
  std::filesystem::create_directories(paths.drive_dir);

  // Seed default profile with a real save.txt + garage.txt.
  {
    drive::DriveSave ds;
    ds.car = "NIS_SkylineGTRVSpecII_02";
    ds.credits = 55000;
    ds.wristband_points = 1200;
    ds.level = 4;
    ds.signs_found = {"flyer_001", "flyer_002"};
    ds.garage = {445};
    std::string err;
    assert(drive::write_save(paths.drive_dir / "save.txt", ds, &err));
    garage::Garage g;
    garage::Car c;
    c.id = 1;
    c.car_id = 445;
    c.performance_index = 0.52;
    c.class_id = 3;
    g.cars.push_back(c);
    g.last_id = 1;
    g.selected = 1;
    assert(garage::write_garage(paths.drive_dir / "garage.txt", g, &err));
  }

  // 1. Scan initial default profile
  auto profiles = launcher::scan_save_profiles(paths);
  assert(profiles.size() == 1);
  assert(profiles[0].name == "default");
  assert(profiles[0].active);
  assert(profiles[0].has_save);
  assert(profiles[0].credits == 55000);
  assert(profiles[0].garage_cars == 1);
  assert(profiles[0].signs_found == 2);

  // 2. Create new profile
  std::string weekend_name, err;
  assert(launcher::create_save_profile(paths, "Weekend Racer", &weekend_name, &err));
  assert(weekend_name == "weekend-racer");

  // 3. Switch active profile (updates both profiles.txt and settings.txt)
  assert(launcher::set_active_save_profile(paths, weekend_name, &err));
  assert(launcher::load_profile_setting(paths.settings_file) == weekend_name);
  profiles = launcher::scan_save_profiles(paths);
  assert(profiles.size() == 2);
  bool weekend_active = false;
  for (const auto& p : profiles) {
    if (p.name == weekend_name) weekend_active = p.active;
  }
  assert(weekend_active);

  // 4. Backup default profile
  std::string backup_id;
  assert(launcher::backup_save_profile(paths, "default", &backup_id, &err));
  assert(!backup_id.empty());
  profiles = launcher::scan_save_profiles(paths);
  assert(profiles[0].backup_count == 1);

  // 5. Duplicate default profile -> copies save.txt & garage.txt
  std::string dup_name;
  assert(launcher::duplicate_save_profile(paths, "default", &dup_name, &err));
  assert(!dup_name.empty());
  profiles = launcher::scan_save_profiles(paths);
  bool found_dup = false;
  for (const auto& p : profiles) {
    if (p.name == dup_name) {
      found_dup = true;
      assert(p.has_save && p.credits == 55000 && p.garage_cars == 1);
    }
  }
  assert(found_dup);

  // 6. Delete refuses default and active, succeeds on inactive duplicate
  assert(!launcher::delete_save_profile(paths, "default", &err));
  assert(!launcher::delete_save_profile(paths, weekend_name, &err));
  assert(launcher::delete_save_profile(paths, dup_name, &err));
  profiles = launcher::scan_save_profiles(paths);
  assert(profiles.size() == 2);

  // 7. PinyonShift auto-detection & strictly read-only import (plaintext export, ZIP, encrypted)
  const auto pinyon_root = root / "pinyon_sources";
  const auto plaintext_dir = pinyon_root / "export_plaintext";
  {
    drive::DriveSave src_ds;
    src_ds.car = "VIP_Viper_13";
    src_ds.credits = 248000;
    src_ds.wristband_points = 6400;
    src_ds.level = 7;
    src_ds.signs_found = {"flyer_001", "flyer_002", "flyer_003"};
    src_ds.garage = {445, 500};
    garage::Garage src_g;
    garage::Car c1;
    c1.id = 1;
    c1.car_id = 445;
    c1.performance_index = 0.55;
    c1.class_id = 3;
    garage::Car c2;
    c2.id = 2;
    c2.car_id = 500;
    c2.performance_index = 0.65;
    c2.class_id = 4;
    src_g.cars = {c1, c2};
    src_g.last_id = 2;
    src_g.selected = 1;
    assert(profile::export_forza_profile(src_ds, src_g, plaintext_dir, &err));
  }
  // Hash plaintext export files before scan/import to verify strictly read-only behavior.
  std::string body_sha_before, db_sha_before;
  assert(launcher::sha256_file(plaintext_dir / "ForzaProfile.body", body_sha_before));
  assert(launcher::sha256_file(plaintext_dir / "PlayerDatabase.sqlite", db_sha_before));

  // Build an export ZIP containing the plaintext export.
  const auto zip_path = pinyon_root / "horizon_export.zip";
  {
    const std::string body_str = read_binary_string(plaintext_dir / "ForzaProfile.body");
    const std::string db_str = read_binary_string(plaintext_dir / "PlayerDatabase.sqlite");
    write_stored_zip_local(
        zip_path,
        {{"user/ForzaProfile.body", std::vector<uint8_t>(body_str.begin(), body_str.end())},
         {"user/PlayerDatabase.sqlite", std::vector<uint8_t>(db_str.begin(), db_str.end())}});
  }

  // Build an encrypted PinyonShift preview user tree (read-only detection test).
  const auto enc_dir = pinyon_root / "source" / "0.1.0" / ".local" / "preview" / "user" / "ForzaProfile";
  std::filesystem::create_directories(enc_dir);
  {
    std::ofstream f(enc_dir / "ForzaProfile", std::ios::binary);
    f << "ENCRYPTED_CONSOLE_BLOB";
  }
  std::string enc_sha_before;
  assert(launcher::sha256_file(enc_dir / "ForzaProfile", enc_sha_before));

  const auto sources = launcher::scan_pinyon_save_sources({pinyon_root});
  bool saw_plaintext = false, saw_zip = false, saw_encrypted = false;
  for (const auto& s : sources) {
    if (s.kind == launcher::PinyonSaveKind::kPlaintextExport && s.importable) saw_plaintext = true;
    if (s.kind == launcher::PinyonSaveKind::kExportZip && s.importable) saw_zip = true;
    if (s.kind == launcher::PinyonSaveKind::kEncryptedPinyon && !s.importable) saw_encrypted = true;
  }
  assert(saw_plaintext && saw_zip && saw_encrypted);

  // Import plaintext export into existing `weekend-racer` profile.
  std::string summary;
  assert(launcher::import_pinyon_save(paths, plaintext_dir, weekend_name, false, true, &summary, &err));
  assert(summary.find("248,000 CR") != std::string::npos);

  // Import ZIP into a brand new profile.
  assert(launcher::import_pinyon_save(paths, zip_path, "Zip Career", true, false, &summary, &err));
  assert(summary.find("248,000 CR") != std::string::npos);
  profiles = launcher::scan_save_profiles(paths);
  assert(profiles.size() == 3);
  bool found_zip_prof = false;
  for (const auto& p : profiles) {
    if (p.gamertag == "Zip Career") {
      found_zip_prof = true;
      assert(p.has_save && p.credits == 248000 && p.garage_cars == 2 && p.signs_found == 3);
    }
  }
  assert(found_zip_prof);

  // Encrypted source must be refused cleanly without creating a new profile.
  const size_t count_before_enc = profiles.size();
  assert(!launcher::import_pinyon_save(paths, enc_dir.parent_path(), "Bad Enc", true, false, &summary, &err));
  assert(!err.empty());
  assert(launcher::scan_save_profiles(paths).size() == count_before_enc);

  // Verify ALL source files are 100% untouched byte-for-byte!
  std::string body_sha_after, db_sha_after, enc_sha_after;
  assert(launcher::sha256_file(plaintext_dir / "ForzaProfile.body", body_sha_after));
  assert(launcher::sha256_file(plaintext_dir / "PlayerDatabase.sqlite", db_sha_after));
  assert(launcher::sha256_file(enc_dir / "ForzaProfile", enc_sha_after));
  assert(body_sha_before == body_sha_after);
  assert(db_sha_before == db_sha_after);
  assert(enc_sha_before == enc_sha_after);

  std::filesystem::remove_all(root);
  std::printf("  [ok] save profile CRUD & read-only PinyonShift export/ZIP/encrypted detection + import\n");
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("launcher_tests:\n");
  test_sha256_and_crc32();
  test_portable_paths();
  test_disc_verifier();
  test_iso_probe_and_setup_extraction();
  test_zerobake_setup_runner();
  test_hardware_advisor();
  test_preset_mapping_and_cores();
  test_launcher_sounds_and_posix_quoting();
  test_graphics_backend_selector();
  test_bidirectional_settings_sync();
  test_save_management_and_pinyon_import();
  test_steam_vdf();
  test_mods_and_dlc();
  test_stfs_tu_v4_and_dlc_install();
  test_radio_wheel_updater();
  test_wifi_sync_server_and_qr();
  std::printf("launcher_tests: all passed.\n");
  return 0;
}
