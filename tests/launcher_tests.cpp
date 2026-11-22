#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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
#include "sha256.h"

namespace {

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

  std::printf("  [ok] hardware capability advisor\n");
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

  // STFS DLC & v4 + UDLC/XE detection
  const auto stfs_src = root / "dlcpak_rally";
  {
    std::ofstream f(stfs_src, std::ios::binary);
    f.write("LIVE", 4);
    f << "payload";
  }
  assert(launcher::is_stfs_package_file(stfs_src));
  std::string imp_msg;
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

}  // namespace

int main() {
  std::printf("launcher_tests:\n");
  test_sha256_and_crc32();
  test_portable_paths();
  test_disc_verifier();
  test_hardware_advisor();
  test_steam_vdf();
  test_mods_and_dlc();
  test_radio_wheel_updater();
  test_wifi_sync_server_and_qr();
  std::printf("launcher_tests: all passed.\n");
  return 0;
}
