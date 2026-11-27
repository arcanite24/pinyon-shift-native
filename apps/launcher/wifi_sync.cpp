// Android Wi-Fi Sync LAN server (HTTP/1.1 + Range + UDP LAN discovery) and
// scannable QR code matrix encoder for Shift Engine Launcher.
#include "launcher_core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using sock_t = SOCKET;
constexpr sock_t kInvalidSock = INVALID_SOCKET;
inline void close_sock(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cctype>
using sock_t = int;
constexpr sock_t kInvalidSock = -1;
inline void close_sock(sock_t s) { close(s); }
#endif

namespace launcher {
namespace {

void ensure_sockets_init() {
#ifdef _WIN32
  static struct WsaInit {
    WsaInit() {
      WSADATA d{};
      WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WsaInit() { WSACleanup(); }
  } init;
  (void)init;
#endif
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string local_hostname() {
  ensure_sockets_init();
  char buf[128]{};
  if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') return buf;
  return "ShiftEnginePC";
}

bool is_private_ipv4(uint32_t host_order_ip) {
  const uint8_t a = uint8_t((host_order_ip >> 24) & 0xFFu);
  const uint8_t b = uint8_t((host_order_ip >> 16) & 0xFFu);
  if (a == 127 || a == 10) return true;
  if (a == 172 && b >= 16 && b <= 31) return true;
  if (a == 192 && b == 168) return true;
  if (a == 169 && b == 254) return true;
  return false;
}

bool constant_time_eq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= uint8_t(a[i] ^ b[i]);
  return diff == 0;
}

void collect_dir_files(const std::filesystem::path& src_root, const std::string& dst_prefix,
                       WifiSyncGroup& group) {
  std::error_code ec;
  if (!std::filesystem::is_directory(src_root, ec)) return;
  for (const auto& e : std::filesystem::recursive_directory_iterator(src_root, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const auto rel = e.path().lexically_relative(src_root).generic_string();
    if (rel.empty() || rel[0] == '.') continue;
    WifiSyncFile f;
    f.source = e.path();
    f.destination = dst_prefix + "/" + rel;
    f.size = std::filesystem::file_size(e.path(), ec);
    const auto ft = std::filesystem::last_write_time(e.path(), ec);
    if (!ec) {
      f.modified_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ft.time_since_epoch()).count();
    }
    group.total_bytes += f.size;
    group.files.push_back(std::move(f));
  }
}

bool send_all(sock_t s, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  size_t sent = 0;
  while (sent < len) {
    const int n = int(send(s, p + sent, int(std::min<size_t>(len - sent, 1 << 16)), 0));
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}

void send_text_response(sock_t s, int code, const char* reason, const char* content_type,
                        const std::string& body, bool head_only) {
  std::ostringstream hdr;
  hdr << "HTTP/1.1 " << code << " " << reason << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Connection: close\r\n\r\n";
  const std::string h = hdr.str();
  send_all(s, h.data(), h.size());
  if (!head_only && !body.empty()) send_all(s, body.data(), body.size());
}

void send_file_response(sock_t s, const std::filesystem::path& file_path, const std::string& range_hdr,
                        const char* content_type, bool head_only, const char* disposition = nullptr) {
  std::ifstream in(file_path, std::ios::binary | std::ios::ate);
  if (!in) {
    send_text_response(s, 404, "Not Found", "text/plain", "File unavailable on host.", head_only);
    return;
  }
  const uint64_t total = uint64_t(in.tellg());
  uint64_t start = 0;
  int status = 200;
  const char* reason = "OK";

  if (range_hdr.rfind("bytes=", 0) == 0) {
    const std::string spec = range_hdr.substr(6);
    const size_t dash = spec.find('-');
    if (dash != std::string::npos && dash > 0) {
      start = std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10);
      if (start >= total && total > 0) {
        std::ostringstream unsat;
        unsat << "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" << total
              << "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        const std::string h = unsat.str();
        send_all(s, h.data(), h.size());
        return;
      }
      status = 206;
      reason = "Partial Content";
    }
  }

  const uint64_t remaining = total > start ? (total - start) : 0;
  std::ostringstream hdr;
  hdr << "HTTP/1.1 " << status << " " << reason << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << remaining << "\r\n"
      << "Accept-Ranges: bytes\r\n"
      << "Cache-Control: no-store\r\n";
  if (status == 206 && total > 0) {
    hdr << "Content-Range: bytes " << start << "-" << (total - 1) << "/" << total << "\r\n";
  }
  if (disposition) hdr << "Content-Disposition: " << disposition << "\r\n";
  hdr << "Connection: close\r\n\r\n";
  const std::string h = hdr.str();
  if (!send_all(s, h.data(), h.size()) || head_only) return;

  in.seekg(std::streamoff(start), std::ios::beg);
  std::vector<char> buf(1 << 16);
  uint64_t left = remaining;
  while (left > 0 && in) {
    const size_t want = size_t(std::min<uint64_t>(left, buf.size()));
    in.read(buf.data(), std::streamsize(want));
    const auto got = in.gcount();
    if (got <= 0) break;
    if (!send_all(s, buf.data(), size_t(got))) break;
    left -= uint64_t(got);
  }
}

// ---- Minimal Real QR Code Generator (Version 3 / 29x29 or Version 4 / 33x33, ECC L) ----

uint8_t gf_mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1u) r ^= a;
    const bool hi = (a & 0x80u) != 0;
    a <<= 1;
    if (hi) a ^= 0x1Du;  // primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D)
    b >>= 1;
  }
  return r;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, int ecc_len) {
  std::vector<uint8_t> gen = {1};
  uint8_t root = 1;
  for (int i = 0; i < ecc_len; ++i) {
    std::vector<uint8_t> next(gen.size() + 1, 0);
    for (size_t j = 0; j < gen.size(); ++j) {
      next[j] ^= gen[j];
      next[j + 1] ^= gf_mul(gen[j], root);
    }
    gen = std::move(next);
    root = gf_mul(root, 2);
  }
  std::vector<uint8_t> rem(size_t(ecc_len), 0);
  for (uint8_t b : data) {
    const uint8_t factor = b ^ rem[0];
    rem.erase(rem.begin());
    rem.push_back(0);
    for (int j = 0; j < ecc_len; ++j) rem[size_t(j)] ^= gf_mul(gen[size_t(j + 1)], factor);
  }
  return rem;
}

}  // namespace

std::string WifiSyncManifest::to_json() const {
  std::ostringstream os;
  os << "{\"schema_version\":1,\"name\":\"" << json_escape(host_name) << "\"";
  if (!apk_path.empty()) {
    std::error_code ec;
    const uint64_t sz = std::filesystem::is_regular_file(apk_path, ec) ? std::filesystem::file_size(apk_path, ec) : 0;
    os << ",\"apk\":{\"filename\":\"shift-drive-arm64.apk\",\"bytes\":" << sz << "}";
  }
  os << ",\"groups\":[";
  size_t file_index = 0;
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    if (gi > 0) os << ",";
    const auto& g = groups[gi];
    os << "{\"id\":\"" << json_escape(g.id) << "\",\"label\":\"" << json_escape(g.label)
       << "\",\"selected\":" << (g.selected ? "true" : "false")
       << ",\"bytes\":" << g.total_bytes << ",\"files\":[";
    for (size_t fi = 0; fi < g.files.size(); ++fi) {
      if (fi > 0) os << ",";
      const auto& f = g.files[fi];
      os << "{\"index\":" << file_index++ << ",\"path\":\"" << json_escape(f.destination)
         << "\",\"size\":" << f.size << ",\"modified\":" << f.modified_ms << "}";
    }
    os << "]}";
  }
  os << "]}";
  return os.str();
}

WifiSyncManifest build_wifi_sync_manifest(const LauncherPaths& paths,
                                          bool include_play,
                                          bool include_dlc,
                                          bool include_mods,
                                          bool include_radio,
                                          const std::filesystem::path& apk_path) {
  WifiSyncManifest m;
  m.host_name = local_hostname();
  m.apk_path = apk_path;

  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<int> code_dist(0, 999999);
  char code_buf[16];
  std::snprintf(code_buf, sizeof code_buf, "%06d", code_dist(rng));
  m.pairing_code = code_buf;

  static constexpr char kHex[] = "0123456789abcdef";
  std::uniform_int_distribution<int> hex_dist(0, 15);
  m.token.resize(16);
  for (char& c : m.token) c = kHex[hex_dist(rng)];

  if (include_play) {
    WifiSyncGroup g;
    g.id = "play";
    g.label = "Play folder (cars, audio, world index)";
    g.selected = true;
    collect_dir_files(paths.play_dir, "play", g);
    m.groups.push_back(std::move(g));
  }
  if (include_dlc) {
    WifiSyncGroup g;
    g.id = "dlc";
    g.label = "DLC and Title Update v4";
    g.selected = true;
    collect_dir_files(paths.state_dir, "state", g);
    m.groups.push_back(std::move(g));
  }
  if (include_mods || include_radio) {
    const auto mods = scan_mods(paths.mods_dir);
    WifiSyncGroup mods_g;
    mods_g.id = "mods";
    mods_g.label = "Enabled mods";
    mods_g.selected = include_mods;
    WifiSyncGroup radio_g;
    radio_g.id = "radio";
    radio_g.label = "Custom radio stations";
    radio_g.selected = include_radio;

    for (const auto& mod : mods) {
      if (!mod.enabled) continue;
      const bool is_radio = std::find(mod.contents.begin(), mod.contents.end(), "custom radio") != mod.contents.end();
      if (is_radio && include_radio) {
        collect_dir_files(mod.dir, "mods/" + mod.name, radio_g);
      } else if (!is_radio && include_mods) {
        collect_dir_files(mod.dir, "mods/" + mod.name, mods_g);
      }
    }
    if (include_mods) m.groups.push_back(std::move(mods_g));
    if (include_radio) m.groups.push_back(std::move(radio_g));
  }
  return m;
}

QrMatrix generate_qr_matrix(std::string_view text) {
  // Choose Version 3 (29x29, 55 data bytes, 15 ECC bytes, L) or Version 4 (33x33, 80 data bytes, 20 ECC bytes, L).
  const int ver = text.size() <= 53 ? 3 : 4;
  const int size = 17 + 4 * ver;
  const int data_codewords = ver == 3 ? 55 : 80;
  const int ecc_codewords = ver == 3 ? 15 : 20;
  const int align_center = ver == 3 ? 22 : 26;

  // Build Byte-mode bitstream
  std::vector<uint8_t> bits;
  auto push_bits = [&](uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bits.push_back(uint8_t((v >> i) & 1u));
  };
  const size_t len = std::min<size_t>(text.size(), size_t(data_codewords - 2));
  push_bits(0b0100, 4);  // Byte mode
  push_bits(uint32_t(len), 8);
  for (size_t i = 0; i < len; ++i) push_bits(uint8_t(text[i]), 8);
  push_bits(0, std::min<int>(4, data_codewords * 8 - int(bits.size())));
  while (bits.size() % 8 != 0) bits.push_back(0);

  std::vector<uint8_t> cw;
  cw.reserve(size_t(data_codewords + ecc_codewords));
  for (size_t i = 0; i < bits.size(); i += 8) {
    uint8_t b = 0;
    for (int j = 0; j < 8; ++j) b = uint8_t((b << 1) | bits[i + size_t(j)]);
    cw.push_back(b);
  }
  static constexpr uint8_t kPad[2] = {0xEC, 0x11};
  int pi = 0;
  while (int(cw.size()) < data_codewords) {
    cw.push_back(kPad[pi & 1]);
    ++pi;
  }
  const auto ecc = rs_remainder(cw, ecc_codewords);
  cw.insert(cw.end(), ecc.begin(), ecc.end());

  QrMatrix qr;
  qr.size = size;
  qr.modules.assign(size_t(size * size), 0);
  std::vector<uint8_t> is_func(size_t(size * size), 0);

  auto set_fn = [&](int x, int y, bool dark) {
    if (x < 0 || y < 0 || x >= size || y >= size) return;
    qr.modules[size_t(y * size + x)] = dark ? 1 : 0;
    is_func[size_t(y * size + x)] = 1;
  };
  auto draw_finder = [&](int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy) {
      for (int dx = -4; dx <= 4; ++dx) {
        const int d = std::max(std::abs(dx), std::abs(dy));
        set_fn(cx + dx, cy + dy, d != 2 && d != 4);
      }
    }
  };
  draw_finder(3, 3);
  draw_finder(size - 4, 3);
  draw_finder(3, size - 4);

  // Alignment pattern
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const int d = std::max(std::abs(dx), std::abs(dy));
      set_fn(align_center + dx, align_center + dy, d != 1);
    }
  }
  // Timing patterns
  for (int i = 8; i < size - 8; ++i) {
    set_fn(i, 6, (i % 2) == 0);
    set_fn(6, i, (i % 2) == 0);
  }
  // Dark module
  set_fn(8, size - 8, true);

  // Format info for ECC L (01) + Mask 0 (000) -> precomputed BCH 15-bit masked: 0x77C4
  constexpr uint32_t kFormatBits = 0x77C4u;
  for (int i = 0; i <= 5; ++i) set_fn(8, i, ((kFormatBits >> i) & 1u) != 0);
  set_fn(8, 7, ((kFormatBits >> 6) & 1u) != 0);
  set_fn(8, 8, ((kFormatBits >> 7) & 1u) != 0);
  set_fn(7, 8, ((kFormatBits >> 8) & 1u) != 0);
  for (int i = 9; i < 15; ++i) set_fn(14 - i, 8, ((kFormatBits >> i) & 1u) != 0);
  for (int i = 0; i < 8; ++i) set_fn(size - 1 - i, 8, ((kFormatBits >> i) & 1u) != 0);
  for (int i = 8; i < 15; ++i) set_fn(8, size - 15 + i, ((kFormatBits >> i) & 1u) != 0);

  // Place data bits zig-zag upward/downward with Mask 0: (x + y) % 2 == 0
  size_t bit_idx = 0;
  bool upward = true;
  for (int right = size - 1; right >= 1; right -= 2) {
    if (right == 6) right = 5;
    for (int step = 0; step < size; ++step) {
      const int y = upward ? (size - 1 - step) : step;
      for (int j = 0; j < 2; ++j) {
        const int x = right - j;
        if (is_func[size_t(y * size + x)]) continue;
        bool dark = false;
        if (bit_idx < cw.size() * 8) {
          dark = ((cw[bit_idx >> 3] >> (7 - (bit_idx & 7))) & 1u) != 0;
          ++bit_idx;
        }
        if (((x + y) & 1) == 0) dark = !dark;  // Mask 0
        qr.modules[size_t(y * size + x)] = dark ? 1 : 0;
      }
    }
    upward = !upward;
  }
  return qr;
}

std::vector<std::string> discover_local_lan_ips() {
  ensure_sockets_init();
  std::vector<std::string> out;
#ifdef _WIN32
  ULONG sz = 16384;
  std::vector<uint8_t> buf(sz);
  auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &sz) ==
      ERROR_BUFFER_OVERFLOW) {
    buf.resize(sz);
    addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
  }
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &sz) ==
      NO_ERROR) {
    for (auto* a = addrs; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
        const uint32_t hip = ntohl(sin->sin_addr.s_addr);
        if (!is_private_ipv4(hip) || (hip >> 24) == 127) continue;
        char ip_str[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof ip_str)) out.emplace_back(ip_str);
      }
    }
  }
#else
  struct ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) == 0) {
    for (auto* p = ifa; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
      auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
      const uint32_t hip = ntohl(sin->sin_addr.s_addr);
      if (!is_private_ipv4(hip) || (hip >> 24) == 127) continue;
      char ip_str[INET_ADDRSTRLEN]{};
      if (inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof ip_str)) out.emplace_back(ip_str);
    }
    freeifaddrs(ifa);
  }
#endif
  if (out.empty()) out.push_back("127.0.0.1");
  return out;
}

WifiSyncServer::~WifiSyncServer() { stop(); }

bool WifiSyncServer::start(const WifiSyncManifest& manifest, const std::string& bind_ip, uint16_t port,
                           bool enable_udp_discovery) {
  stop();
  ensure_sockets_init();
  manifest_ = manifest;
  flat_files_.clear();
  for (const auto& g : manifest_.groups) {
    for (const auto& f : g.files) flat_files_.push_back(f);
  }
  wrong_codes_.store(0);

  sock_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == kInvalidSock) return false;
  int yes = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (bind_ip.empty() || bind_ip == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);
  }
  if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    // Fall back to ephemeral port if preferred port is busy.
    addr.sin_port = 0;
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
      close_sock(ls);
      return false;
    }
  }
  if (listen(ls, 8) != 0) {
    close_sock(ls);
    return false;
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof bound;
  getsockname(ls, reinterpret_cast<sockaddr*>(&bound), &blen);
  port_.store(ntohs(bound.sin_port));
  listen_sock_ = intptr_t(ls);
  running_.store(true);

  log_activity("Wi-Fi Sync started on port " + std::to_string(port_.load()) + " (code " + manifest_.pairing_code + ")");
  accept_thread_ = std::thread(&WifiSyncServer::accept_loop, this);

  if (enable_udp_discovery && bind_ip != "127.0.0.1") {
    sock_t us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (us != kInvalidSock) {
      setsockopt(us, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
      setsockopt(us, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
      sockaddr_in uaddr{};
      uaddr.sin_family = AF_INET;
      uaddr.sin_port = htons(kDiscoveryPort);
      uaddr.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(us, reinterpret_cast<sockaddr*>(&uaddr), sizeof uaddr) == 0) {
        udp_sock_ = intptr_t(us);
        discovery_thread_ = std::thread(&WifiSyncServer::discovery_loop, this);
      } else {
        close_sock(us);
      }
    }
  }
  return true;
}

void WifiSyncServer::stop() {
  if (!running_.exchange(false)) return;
  if (listen_sock_ != -1) {
    close_sock(sock_t(listen_sock_));
    listen_sock_ = -1;
  }
  if (udp_sock_ != -1) {
    close_sock(sock_t(udp_sock_));
    udp_sock_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  if (discovery_thread_.joinable()) discovery_thread_.join();
  log_activity("Wi-Fi Sync stopped.");
}

std::string WifiSyncServer::pairing_code() const { return manifest_.pairing_code; }

std::string WifiSyncServer::download_url(const std::string& host_ip) const {
  return "http://" + host_ip + ":" + std::to_string(port_.load()) + "/" + manifest_.token;
}

std::vector<std::string> WifiSyncServer::recent_activity() const {
  std::lock_guard<std::mutex> lk(log_mu_);
  return activity_;
}

void WifiSyncServer::log_activity(const std::string& msg) {
  std::lock_guard<std::mutex> lk(log_mu_);
  activity_.push_back(msg);
  if (activity_.size() > 32) activity_.erase(activity_.begin());
}

void WifiSyncServer::discovery_loop() {
  const std::string reply = "SHIFT-ENGINE-HERE 1 " + std::to_string(port_.load()) + " " + manifest_.host_name;
  char buf[256];
  while (running_.load()) {
    sockaddr_in from{};
    socklen_t flen = sizeof from;
    const int n = int(recvfrom(sock_t(udp_sock_), buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<sockaddr*>(&from), &flen));
    if (n <= 0) break;
    buf[n] = '\0';
    if (!is_private_ipv4(ntohl(from.sin_addr.s_addr))) continue;
    std::string req(buf);
    while (!req.empty() && (req.back() == '\r' || req.back() == '\n' || req.back() == ' ')) req.pop_back();
    if (req == "SHIFT-ENGINE-DISCOVER 1" || req == "PINYON-SHIFT-DISCOVER 1") {
      sendto(sock_t(udp_sock_), reply.data(), int(reply.size()), 0,
             reinterpret_cast<sockaddr*>(&from), flen);
    }
  }
}

void WifiSyncServer::accept_loop() {
  while (running_.load()) {
    sockaddr_in from{};
    socklen_t flen = sizeof from;
    sock_t client = accept(sock_t(listen_sock_), reinterpret_cast<sockaddr*>(&from), &flen);
    if (client == kInvalidSock) break;
    char ip_str[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof ip_str);
    if (!is_private_ipv4(ntohl(from.sin_addr.s_addr))) {
      send_text_response(client, 403, "Forbidden", "text/plain", "LAN only.", false);
      close_sock(client);
      continue;
    }
    handle_client(intptr_t(client), ip_str);
    close_sock(client);
  }
}

void WifiSyncServer::handle_client(intptr_t client_sock, const std::string& remote_ip) {
  sock_t s = sock_t(client_sock);
  char buf[4096];
  size_t total = 0;
  std::string raw;
  while (total < sizeof(buf) - 1) {
    const int n = int(recv(s, buf + total, int(sizeof(buf) - 1 - total), 0));
    if (n <= 0) return;
    total += size_t(n);
    buf[total] = '\0';
    if (std::strstr(buf, "\r\n\r\n") != nullptr) break;
  }
  raw.assign(buf, total);
  const size_t eol = raw.find("\r\n");
  if (eol == std::string::npos) return;
  const std::string req_line = raw.substr(0, eol);
  std::istringstream rls(req_line);
  std::string method, path;
  rls >> method >> path;
  if (method != "GET" && method != "HEAD") {
    send_text_response(s, 405, "Method Not Allowed", "text/plain", "GET/HEAD only.", false);
    return;
  }
  const bool head_only = (method == "HEAD");
  if (const size_t q = path.find('?'); q != std::string::npos) path = path.substr(0, q);
  while (path.size() > 1 && path.back() == '/') path.pop_back();

  // Parse headers case-insensitively
  std::string code_hdr, range_hdr;
  size_t pos = eol + 2;
  while (pos < raw.size()) {
    const size_t next = raw.find("\r\n", pos);
    if (next == std::string::npos || next == pos) break;
    const std::string line = raw.substr(pos, next - pos);
    if (const size_t col = line.find(':'); col != std::string::npos) {
      std::string k = line.substr(0, col);
      for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
      std::string v = line.substr(col + 1);
      while (!v.empty() && v.front() == ' ') v.erase(v.begin());
      if (k == "x-shift-code" || k == "x-pinyon-code") code_hdr = v;
      else if (k == "range") range_hdr = v;
    }
    pos = next + 2;
  }

  if (path == "/" + manifest_.token) {
    log_activity(remote_ip + " opened APK download page");
    const std::string html =
        "<!doctype html><html><head><meta charset='utf-8'><title>Shift Engine Android Sync</title></head>"
        "<body style='font-family:sans-serif;background:#14161a;color:#f5f6fa;padding:24px'>"
        "<h1>Shift Engine for Android</h1>"
        "<p>Host: " + manifest_.host_name + "</p>"
        "<p><a style='color:#f5045c;font-size:20px' href='/" + manifest_.token +
        "/shift-drive-arm64.apk'>Download shift-drive-arm64.apk</a></p>"
        "<p>Open Shift Drive on Android and enter the 6-digit pairing code shown in the launcher.</p></body></html>";
    send_text_response(s, 200, "OK", "text/html; charset=utf-8", html, head_only);
    return;
  }
  if (path == "/" + manifest_.token + "/shift-drive-arm64.apk") {
    log_activity(remote_ip + " downloading APK");
    send_file_response(s, manifest_.apk_path, range_hdr, "application/vnd.android.package-archive",
                       head_only, "attachment; filename=\"shift-drive-arm64.apk\"");
    return;
  }
  if (path == "/shift/v1/hello") {
    const std::string body = "{\"name\":\"" + json_escape(manifest_.host_name) + "\",\"protocol\":1}";
    send_text_response(s, 200, "OK", "application/json", body, head_only);
    return;
  }
  if (path.rfind("/shift/v1/", 0) != 0) {
    send_text_response(s, 404, "Not Found", "text/plain", "Not found.", head_only);
    return;
  }
  if (locked()) {
    send_text_response(s, 403, "Forbidden", "text/plain", "Locked after too many wrong pairing codes.", head_only);
    return;
  }
  if (!constant_time_eq(code_hdr, manifest_.pairing_code)) {
    wrong_codes_.fetch_add(1);
    log_activity(remote_ip + " sent invalid pairing code");
    send_text_response(s, 403, "Forbidden", "text/plain", "Wrong pairing code.", head_only);
    return;
  }
  if (path == "/shift/v1/manifest") {
    log_activity(remote_ip + " paired and fetched manifest");
    send_text_response(s, 200, "OK", "application/json", manifest_.to_json(), head_only);
    return;
  }
  if (path.rfind("/shift/v1/file/", 0) == 0) {
    const std::string idx_s = path.substr(15);
    char* endp = nullptr;
    const unsigned long idx = std::strtoul(idx_s.c_str(), &endp, 10);
    if (endp != idx_s.c_str() && *endp == '\0' && idx < flat_files_.size()) {
      send_file_response(s, flat_files_[idx].source, range_hdr, "application/octet-stream", head_only);
      return;
    }
  }
  send_text_response(s, 404, "Not Found", "text/plain", "Not found.", head_only);
}

}  // namespace launcher
