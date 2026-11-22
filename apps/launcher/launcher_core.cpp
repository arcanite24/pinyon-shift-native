#include "launcher_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(SHIFT_LAUNCHER_HAS_VOLK)
#include <volk.h>
#endif

#include "sha256.h"

namespace launcher {
namespace {

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string trim(std::string_view s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

std::string strip_quotes(std::string_view s) {
  std::string t = trim(s);
  if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
    return t.substr(1, t.size() - 2);
  }
  return t;
}

std::string xml_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string format_bytes(uint64_t bytes) {
  char buf[64];
  if (bytes >= (1ull << 30)) {
    std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
  } else if (bytes >= (1ull << 20)) {
    std::snprintf(buf, sizeof buf, "%.0f MB", double(bytes) / double(1ull << 20));
  } else if (bytes >= (1ull << 10)) {
    std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / double(1ull << 10));
  } else {
    std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
  }
  return buf;
}

std::filesystem::path find_program_near(const std::filesystem::path& launcher_dir,
                                        const std::string& base_name,
                                        const std::string& exe_suffix) {
  std::error_code ec;
  const std::string fname = base_name + exe_suffix;
  const std::filesystem::path candidates[] = {
      launcher_dir / fname,
      launcher_dir.parent_path() / base_name / fname,
      launcher_dir / "apps" / base_name / fname,
  };
  for (const auto& c : candidates) {
    if (!c.empty() && std::filesystem::is_regular_file(c, ec)) return c;
  }
  return {};
}

std::filesystem::path find_in_path(const std::string& name) {
  const char* env_path = std::getenv("PATH");
  if (!env_path) return {};
#ifdef _WIN32
  constexpr char kSep = ';';
  const std::vector<std::string> exts = {"", ".exe", ".cmd", ".bat"};
#else
  constexpr char kSep = ':';
  const std::vector<std::string> exts = {""};
#endif
  std::string path_str(env_path);
  std::error_code ec;
  size_t pos = 0;
  while (pos <= path_str.size()) {
    const size_t next = path_str.find(kSep, pos);
    const std::string dir = path_str.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
    if (!dir.empty()) {
      for (const auto& ext : exts) {
        const auto p = std::filesystem::path(dir) / (name + ext);
        if (std::filesystem::is_regular_file(p, ec)) return p;
      }
    }
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  return {};
}

int query_physical_cores() {
#ifdef _WIN32
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len == 0) return 0;
  std::vector<uint8_t> buf(len);
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                                        &len)) {
    return 0;
  }
  int cores = 0;
  DWORD off = 0;
  while (off + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= len) {
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
    if (info->Size == 0) break;
    if (info->Relationship == RelationProcessorCore) ++cores;
    off += info->Size;
  }
  return cores;
#else
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? int(hw) : 0;
#endif
}

std::string quote_arg_shell(const std::string& a) {
  if (a.find_first_of(" \t\"") == std::string::npos) return a;
  std::string out = "\"";
  for (char c : a) {
    if (c == '"') out += "\\\"";
    else out += c;
  }
  out += '"';
  return out;
}

uint32_t read_le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t read_le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t read_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
void write_le16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v & 0xFFu));
  out.push_back(uint8_t((v >> 8) & 0xFFu));
}
void write_le32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v & 0xFFu));
  out.push_back(uint8_t((v >> 8) & 0xFFu));
  out.push_back(uint8_t((v >> 16) & 0xFFu));
  out.push_back(uint8_t((v >> 24) & 0xFFu));
}

// ---- Minimal JSON parser for GitHub Releases API ---------------------------

struct JsonVal {
  enum Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = kNull;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JsonVal> arr;
  std::map<std::string, JsonVal> obj;

  const JsonVal* get(const std::string& k) const {
    if (kind != kObject) return nullptr;
    auto it = obj.find(k);
    return it == obj.end() ? nullptr : &it->second;
  }
  std::string as_string(const std::string& def = {}) const { return kind == kString ? str : def; }
  bool as_bool(bool def = false) const { return kind == kBool ? b : def; }
  uint64_t as_u64(uint64_t def = 0) const { return kind == kNumber && num >= 0 ? uint64_t(num) : def; }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view s) : s_(s) {}
  bool parse(JsonVal& out) {
    skip_ws();
    if (!parse_val(out)) return false;
    skip_ws();
    return pos_ == s_.size();
  }

 private:
  void skip_ws() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
  }
  bool parse_val(JsonVal& v) {
    skip_ws();
    if (pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '"') {
      v.kind = JsonVal::kString;
      return parse_string(v.str);
    }
    if (c == '{') return parse_obj(v);
    if (c == '[') return parse_arr(v);
    if (c == 't' && s_.substr(pos_, 4) == "true") {
      pos_ += 4;
      v.kind = JsonVal::kBool;
      v.b = true;
      return true;
    }
    if (c == 'f' && s_.substr(pos_, 5) == "false") {
      pos_ += 5;
      v.kind = JsonVal::kBool;
      v.b = false;
      return true;
    }
    if (c == 'n' && s_.substr(pos_, 4) == "null") {
      pos_ += 4;
      v.kind = JsonVal::kNull;
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      const size_t start = pos_;
      if (s_[pos_] == '-') ++pos_;
      while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
                                  s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
        ++pos_;
      }
      v.kind = JsonVal::kNumber;
      v.num = std::strtod(std::string(s_.substr(start, pos_ - start)).c_str(), nullptr);
      return true;
    }
    return false;
  }
  bool parse_string(std::string& out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    out.clear();
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= s_.size()) return false;
        const char e = s_[pos_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u':
            if (pos_ + 4 > s_.size()) return false;
            pos_ += 4;
            out += '?';
            break;
          default: out += e; break;
        }
      } else {
        out += c;
      }
    }
    return false;
  }
  bool parse_arr(JsonVal& v) {
    ++pos_;  // '['
    v.kind = JsonVal::kArray;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (pos_ < s_.size()) {
      JsonVal elem;
      if (!parse_val(elem)) return false;
      v.arr.push_back(std::move(elem));
      skip_ws();
      if (pos_ < s_.size() && s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < s_.size() && s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
    return false;
  }
  bool parse_obj(JsonVal& v) {
    ++pos_;  // '{'
    v.kind = JsonVal::kObject;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (pos_ < s_.size()) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') return false;
      ++pos_;
      JsonVal val;
      if (!parse_val(val)) return false;
      v.obj[std::move(key)] = std::move(val);
      skip_ws();
      if (pos_ < s_.size() && s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < s_.size() && s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
    return false;
  }

  std::string_view s_;
  size_t pos_ = 0;
};

}  // namespace

// ---- 1. Portable Mode & Cross-Platform Paths -------------------------------

LauncherPaths resolve_launcher_paths(const std::filesystem::path& launcher_dir,
                                     bool force_portable,
                                     const std::string& os_override) {
  LauncherPaths p;
  std::error_code ec;
  p.launcher_dir = launcher_dir.empty() ? std::filesystem::current_path(ec) : std::filesystem::weakly_canonical(launcher_dir, ec);
  if (p.launcher_dir.empty()) p.launcher_dir = launcher_dir;

  std::string os = lower_ascii(os_override);
  if (os.empty()) {
#if defined(_WIN32)
    os = "windows";
#elif defined(__APPLE__)
    os = "macos";
#else
    os = "linux";
#endif
  }
  const std::string exe = (os == "windows") ? ".exe" : "";

  // On macOS, portable.txt sits beside Shift Engine.app (two levels above Contents/MacOS).
  std::filesystem::path portable_anchor = p.launcher_dir;
  if (os == "macos" && p.launcher_dir.filename() == "MacOS" &&
      p.launcher_dir.parent_path().filename() == "Contents") {
    portable_anchor = p.launcher_dir.parent_path().parent_path().parent_path();
  }

  p.portable = force_portable || std::filesystem::exists(portable_anchor / "portable.txt", ec) ||
               std::filesystem::exists(p.launcher_dir / "portable.txt", ec);

  if (p.portable) {
    p.data_root = portable_anchor / "data";
    p.path_length_warning = check_portable_path_length(p.data_root);
  } else if (os == "windows") {
    const char* appdata = std::getenv("APPDATA");
    const std::filesystem::path base = appdata && *appdata ? std::filesystem::path(appdata)
                                                           : std::filesystem::path("AppData") / "Roaming";
    p.data_root = base / "shift-engine";
  } else if (os == "macos") {
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = home && *home ? std::filesystem::path(home) : std::filesystem::path("~");
    p.data_root = base / "Library" / "Application Support" / "shift-engine";
  } else {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base =
        (xdg && *xdg) ? std::filesystem::path(xdg)
                      : (home && *home ? std::filesystem::path(home) / ".local" / "share" : std::filesystem::path("."));
    p.data_root = base / "shift-engine";
  }

  p.play_dir = p.data_root / "play";
  p.drive_dir = p.data_root / "drive";
  p.mods_dir = p.data_root / "mods";
  p.state_dir = p.data_root / "state";
  p.settings_file = p.data_root / "settings.txt";
  p.logs_dir = p.data_root / "logs";

  p.carview_bin = find_program_near(p.launcher_dir, "carview", exe);
  p.carbake_bin = find_program_near(p.launcher_dir, "carbake", exe);
  p.soundbake_bin = find_program_near(p.launcher_dir, "soundbake", exe);
  p.ffmpeg_bin = find_in_path("ffmpeg");
  return p;
}

std::string check_portable_path_length(const std::filesystem::path& data_root, int max_chars) {
  const std::string s = data_root.string();
  if (int(s.size()) <= max_chars) return {};
  return "Portable data folder is " + std::to_string(s.size()) + " chars (keep <= " + std::to_string(max_chars) +
         " chars on Windows to stay under MAX_PATH).";
}

bool read_play_config(const std::filesystem::path& play_dir, PlayConfig& out) {
  out = PlayConfig{};
  std::ifstream in(play_dir / "play.txt");
  if (!in) return false;
  for (std::string line; std::getline(in, line);) {
    line = trim(line);
    if (line.rfind("game ", 0) == 0) out.game_root = std::filesystem::path(trim(line.substr(5)));
    else if (line.rfind("state ", 0) == 0) out.state_root = std::filesystem::path(trim(line.substr(6)));
  }
  std::error_code ec;
  const auto cars_dir = play_dir / "bake" / "cars";
  if (std::filesystem::is_directory(cars_dir, ec)) {
    for (const auto& e : std::filesystem::directory_iterator(cars_dir, ec)) {
      if (e.is_regular_file(ec) && e.path().extension() == ".shiftbake") ++out.baked_car_count;
    }
  }
  out.has_colorado_index = std::filesystem::is_regular_file(play_dir / "world" / "colorado.index", ec);
  out.has_sound_dir = std::filesystem::is_directory(play_dir / "sound", ec);
  return !out.game_root.empty();
}

bool write_play_config(const std::filesystem::path& play_dir, const std::filesystem::path& game_root,
                       const std::filesystem::path& state_root, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(play_dir, ec);
  std::ofstream out(play_dir / "play.txt", std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "Cannot write " + (play_dir / "play.txt").string();
    return false;
  }
  out << "game " << game_root.string() << "\n";
  if (!state_root.empty()) out << "state " << state_root.string() << "\n";
  return bool(out);
}

// ---- 2. One-Click Setup & Disc Verification --------------------------------

DiscVerification verify_disc_folder(const std::filesystem::path& selected_path,
                                    const std::filesystem::path& target_play_dir) {
  DiscVerification res;
  std::error_code ec;
  if (selected_path.empty()) {
    res.summary = "Select your extracted Forza Horizon disc folder (holding media/).";
    return res;
  }

  if (std::filesystem::is_regular_file(selected_path, ec) &&
      lower_ascii(selected_path.extension().string()) == ".iso") {
    res.looks_like_iso = true;
    res.summary = "ISO image selected. Extract the Xbox 360 ISO (e.g. with extract-xiso) into a folder holding media/, then select that folder.";
    return res;
  }

  std::filesystem::path root = selected_path;
  if (lower_ascii(root.filename().string()) == "media") root = root.parent_path();
  res.resolved_root = root;

  struct Spec {
    const char* label;
    const char* rel;
    bool required;
    bool hash_it;
  };
  constexpr Spec kSpecs[] = {
      {"Colorado World Archive", "media/tracks/colorado/bin.zip", true, false},
      {"Game Database (gamedb.slt)", "media/db/gamedb.slt", true, true},
      {"Radio Music Bank", "media/audio/radio/Radio_Music.fsb", false, false},
      {"UI4 Scenes Archive", "media/ui/UI.zip", false, false},
      {"Retail Executable (default.xex)", "default.xex", false, false},
  };

  bool all_required = true;
  for (const auto& sp : kSpecs) {
    DiscCheckItem item;
    item.label = sp.label;
    item.relative_path = sp.rel;
    item.required = sp.required;
    const auto full = root / sp.rel;
    if (std::filesystem::is_regular_file(full, ec)) {
      item.present = true;
      item.bytes = std::filesystem::file_size(full, ec);
      if (sp.hash_it) {
        std::string hex;
        if (sha256_file(full, hex)) {
          item.sha256_prefix = hex.substr(0, 16);
          res.db_sha256 = hex;
        }
      }
    } else if (sp.required) {
      all_required = false;
    }
    res.items.push_back(std::move(item));
  }

  const auto cars_dir = root / "media" / "cars";
  DiscCheckItem cars_item;
  cars_item.label = "Vehicle Archives (media/cars/)";
  cars_item.relative_path = "media/cars";
  cars_item.required = true;
  if (std::filesystem::is_directory(cars_dir, ec)) {
    for (const auto& e : std::filesystem::directory_iterator(cars_dir, ec)) {
      if (e.is_regular_file(ec) || e.is_directory(ec)) ++res.car_archive_count;
    }
    cars_item.present = res.car_archive_count > 0;
  }
  if (!cars_item.present) all_required = false;
  res.items.push_back(std::move(cars_item));

  const auto space_target = !target_play_dir.empty() && std::filesystem::exists(target_play_dir, ec)
                                ? target_play_dir
                                : (std::filesystem::exists(root, ec) ? root : std::filesystem::current_path(ec));
  const auto sp_info = std::filesystem::space(space_target, ec);
  if (!ec) {
    res.free_space_bytes = sp_info.available;
    res.sufficient_space = res.free_space_bytes >= (8ull << 30);
  }

  res.valid = all_required;
  if (res.valid) {
    res.summary = "Verified disc folder (" + std::to_string(res.car_archive_count) + " car entries, " +
                  format_bytes(res.free_space_bytes) + " free). Ready for One-Click Setup.";
  } else {
    res.summary = "Missing required files under " + root.string() + " (need media/tracks/colorado/bin.zip, media/db/gamedb.slt, media/cars/).";
  }
  return res;
}

SetupRunner::~SetupRunner() {
  cancel();
  if (worker_.joinable()) worker_.join();
}

void SetupRunner::append_log(const std::string& line) {
  std::lock_guard<std::mutex> lk(mu_);
  state_.log_lines.push_back(line);
  if (state_.log_lines.size() > 250) {
    state_.log_lines.erase(state_.log_lines.begin(), state_.log_lines.begin() + 50);
  }
}

void SetupRunner::set_stage(int idx, const std::string& label, float frac) {
  std::lock_guard<std::mutex> lk(mu_);
  state_.stage_index = idx;
  state_.stage_label = label;
  state_.fraction = std::clamp(frac, 0.0f, 1.0f);
}

int SetupRunner::run_subprocess_logged(const std::vector<std::string>& args) {
  if (args.empty()) return -1;
  std::string cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) cmd += ' ';
    cmd += quote_arg_shell(args[i]);
  }
  append_log("$ " + cmd);
#ifdef _WIN32
  std::string wrapped = cmd + " 2>&1";
  FILE* pipe = _popen(wrapped.c_str(), "r");
#else
  std::string wrapped = cmd + " 2>&1";
  FILE* pipe = popen(wrapped.c_str(), "r");
#endif
  if (!pipe) {
    append_log("ERROR: failed to launch subprocess");
    return -1;
  }
  char line[512];
  while (std::fgets(line, sizeof line, pipe)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) append_log(s);
    if (cancel_.load()) break;
  }
#ifdef _WIN32
  return _pclose(pipe);
#else
  int status = pclose(pipe);
  return WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif
}

bool SetupRunner::start(const LauncherPaths& paths, const SetupOptions& opts) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_.running) return false;
    state_ = SetupProgress{};
    state_.running = true;
    state_.stage_count = 4;
    state_.stage_label = "Verifying disc files...";
    state_.fraction = 0.02f;
  }
  cancel_.store(false);
  if (worker_.joinable()) worker_.join();

  worker_ = std::thread([this, paths, opts]() {
    auto fail = [&](const std::string& msg) {
      append_log("SETUP FAILED: " + msg);
      std::lock_guard<std::mutex> lk(mu_);
      state_.running = false;
      state_.finished = true;
      state_.success = false;
      state_.error_text = msg;
      state_.stage_label = "Failed";
    };

    const auto check = verify_disc_folder(opts.game_root, opts.play_dir);
    if (!check.valid) {
      fail(check.summary);
      return;
    }
    append_log("Disc verified: " + check.resolved_root.string());
    if (!check.db_sha256.empty()) append_log("gamedb.slt SHA-256: " + check.db_sha256.substr(0, 16) + "...");

    std::error_code ec;
    std::filesystem::create_directories(opts.play_dir / "world", ec);
    if (paths.carbake_bin.empty()) {
      fail("carbake binary not found beside launcher.");
      return;
    }

    // Stage 1: carbake
    set_stage(1, "Baking cars with carbake...", 0.15f);
    std::vector<std::string> cb_cmd = {
        paths.carbake_bin.string(), "--game-root", check.resolved_root.string(),
        "--out", (opts.play_dir / "bake").string()};
    if (!opts.state_root.empty()) {
      cb_cmd.push_back("--state-root");
      cb_cmd.push_back(opts.state_root.string());
    }
    if (!opts.cars_filter.empty()) {
      cb_cmd.push_back("--cars");
      cb_cmd.push_back(opts.cars_filter);
    }
    if (run_subprocess_logged(cb_cmd) != 0) {
      fail("carbake returned a non-zero exit code.");
      return;
    }
    if (cancel_.load()) {
      fail("Cancelled by user.");
      return;
    }

    // Stage 2: play.txt
    set_stage(2, "Persisting play.txt...", 0.55f);
    std::string werr;
    if (!write_play_config(opts.play_dir, check.resolved_root, opts.state_root, &werr)) {
      fail(werr);
      return;
    }
    append_log("Wrote " + (opts.play_dir / "play.txt").string());

    // Stage 3: soundbake (optional if FFmpeg is missing)
    set_stage(3, "Baking engine sounds (soundbake)...", 0.70f);
    if (!paths.soundbake_bin.empty()) {
      std::vector<std::string> sb_cmd = {
          paths.soundbake_bin.string(), "--game-root", check.resolved_root.string(),
          "--out", (opts.play_dir / "sound").string()};
      if (!opts.state_root.empty()) {
        sb_cmd.push_back("--state-root");
        sb_cmd.push_back(opts.state_root.string());
      }
      if (opts.all_sounds) {
        sb_cmd.push_back("--all");
      } else {
        sb_cmd.push_back("--cars");
        sb_cmd.push_back("NIS_SkylineGTRVSpecII_02");
      }
      if (run_subprocess_logged(sb_cmd) != 0) {
        append_log("Note: soundbake did not complete (install FFmpeg for offline sound bake). Continuing...");
      }
    } else {
      append_log("Note: soundbake not found; runtime audio decode will be used.");
    }
    if (cancel_.load()) {
      fail("Cancelled by user.");
      return;
    }

    // Stage 4: Colorado index
    if (opts.build_index && !paths.carview_bin.empty()) {
      set_stage(4, "Building Colorado world index...", 0.88f);
      const auto probe_png = opts.play_dir / "world" / "setup-check.png";
      std::vector<std::string> idx_cmd = {
          paths.carview_bin.string(), "--drive", "--headless", "--play", opts.play_dir.string(),
          "--run", "cruise", "--seconds", "0.2", "--every", "1", "--no-sound",
          "--out", probe_png.string()};
      if (run_subprocess_logged(idx_cmd) != 0) {
        fail("Colorado index build failed.");
        return;
      }
      for (const auto& f : std::filesystem::directory_iterator(opts.play_dir / "world", ec)) {
        if (f.path().filename().string().rfind("setup-check", 0) == 0) {
          std::filesystem::remove(f.path(), ec);
        }
      }
    }

    append_log("One-Click Setup complete! Ready to drive.");
    std::lock_guard<std::mutex> lk(mu_);
    state_.running = false;
    state_.finished = true;
    state_.success = true;
    state_.fraction = 1.0f;
    state_.stage_label = "Ready to drive";
  });
  return true;
}

void SetupRunner::cancel() { cancel_.store(true); }

SetupProgress SetupRunner::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_;
}

// ---- 3. Hardware Capability Advisor ("This PC") ----------------------------

std::string GpuInfo::vulkan_version_string() const {
  const uint32_t major = (api_version >> 22) & 0x7Fu;
  const uint32_t minor = (api_version >> 12) & 0x3FFu;
  return std::to_string(major) + "." + std::to_string(minor);
}

bool GpuInfo::supports_vulkan_1_3() const {
  const uint32_t major = (api_version >> 22) & 0x7Fu;
  const uint32_t minor = (api_version >> 12) & 0x3FFu;
  return major > 1 || (major == 1 && minor >= 3);
}

bool GpuInfo::supports_vulkan_1_1() const {
  const uint32_t major = (api_version >> 22) & 0x7Fu;
  const uint32_t minor = (api_version >> 12) & 0x3FFu;
  return major > 1 || (major == 1 && minor >= 1);
}

std::string vendor_driver_url(uint32_t vendor_id) {
  switch (vendor_id) {
    case 0x1002: return "https://www.amd.com/en/support/download/drivers.html";
    case 0x10DE: return "https://www.nvidia.com/en-us/drivers/";
    case 0x8086: return "https://www.intel.com/content/www/us/en/download-center/home.html";
    default: return "https://vulkan.lunarg.com/";
  }
}

HardwareInfo probe_hardware(int refresh_hz) {
  HardwareInfo info;
  info.logical_threads = int(std::thread::hardware_concurrency());
  info.physical_cores = query_physical_cores();
  if (info.physical_cores <= 0) info.physical_cores = std::max(1, info.logical_threads / 2);
  info.refresh_hz = refresh_hz > 0 ? refresh_hz : 60;

#if defined(SHIFT_LAUNCHER_HAS_VOLK)
  if (volkInitialize() == VK_SUCCESS) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "shift-launcher";
    app.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> exts;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
#if defined(__APPLE__)
    uint32_t ext_count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr) == VK_SUCCESS && ext_count > 0) {
      std::vector<VkExtensionProperties> avail(ext_count);
      vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, avail.data());
      for (const auto& e : avail) {
        if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
          exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
          ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
          break;
        }
      }
    }
#endif
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) == VK_SUCCESS) {
      volkLoadInstance(inst);
      uint32_t count = 0;
      if (vkEnumeratePhysicalDevices(inst, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkPhysicalDevice> devs(count);
        if (vkEnumeratePhysicalDevices(inst, &count, devs.data()) == VK_SUCCESS) {
          std::optional<GpuInfo> best;
          for (VkPhysicalDevice d : devs) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(d, &props);
            VkPhysicalDeviceMemoryProperties mem{};
            vkGetPhysicalDeviceMemoryProperties(d, &mem);
            uint64_t vram = 0;
            for (uint32_t h = 0; h < mem.memoryHeapCount; ++h) {
              if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                vram = std::max<uint64_t>(vram, mem.memoryHeaps[h].size);
              }
            }
            GpuInfo g;
            g.name = props.deviceName;
            g.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            g.software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            g.vram_bytes = vram;
            g.api_version = props.apiVersion;
            g.vendor_id = props.vendorID;
            if (!best || (g.discrete && !best->discrete) ||
                (!g.software && best->software) ||
                (g.discrete == best->discrete && g.vram_bytes > best->vram_bytes)) {
              best = std::move(g);
            }
          }
          info.gpu = std::move(best);
        }
      }
      vkDestroyInstance(inst, nullptr);
    }
  }
#endif
  return info;
}

HardwareRecommendation recommend_preset(const HardwareInfo& info) {
  HardwareRecommendation rec;
  if (!info.gpu) {
    rec.warning = "No Vulkan GPU detected. Install graphics drivers from AMD, Intel, or NVIDIA.";
    rec.driver_url = vendor_driver_url(0);
    rec.preset_id = "low";
    rec.carview_preset = "low";
    rec.label = "Low-Spec 60";
    rec.reason = "Fallback preset until a Vulkan 1.3 GPU driver is detected.";
    return rec;
  }

  const GpuInfo& gpu = *info.gpu;
  rec.driver_url = vendor_driver_url(gpu.vendor_id);
  if (gpu.software) {
    rec.warning = "Only a software Vulkan device (" + gpu.name + ") was found; install a hardware GPU driver.";
  } else if (!gpu.supports_vulkan_1_3()) {
    rec.warning = "Shift Engine requires Vulkan 1.3; this driver reports Vulkan " + gpu.vulkan_version_string() +
                  ". Update your GPU driver.";
  }

  constexpr uint64_t kCapableVram = 6ull << 30;
  if (gpu.discrete && !gpu.software && gpu.vram_bytes >= kCapableVram &&
      info.logical_threads >= 6 && info.refresh_hz >= 119) {
    rec.preset_id = "performance_120";
    rec.carview_preset = "high";
    rec.label = "Performance 120";
    rec.reason = "Discrete GPU (>=6 GB VRAM), 6+ CPU threads, and 120+ Hz display.";
    return rec;
  }
  if (gpu.discrete && !gpu.software && gpu.vram_bytes >= (4ull << 30) && info.physical_cores >= 4) {
    rec.preset_id = "quality_60";
    rec.carview_preset = "high";
    rec.label = "Quality 60";
    rec.reason = "Discrete GPU with 4+ GB VRAM and 4+ CPU cores: 4x MSAA, shadows, bloom.";
    return rec;
  }
  if (!gpu.discrete && info.physical_cores > 0 && info.physical_cores <= 2) {
    rec.preset_id = "ultra_low_30";
    rec.carview_preset = "low";
    rec.label = "Ultra Low 30";
    rec.reason = "Integrated graphics on " + std::to_string(info.physical_cores) + " CPU cores: 30 fps without shadows.";
    return rec;
  }
  if (info.physical_cores > 0 && info.physical_cores < 4) {
    rec.preset_id = "balanced_40";
    rec.carview_preset = "low";
    rec.label = "Balanced 40";
    rec.reason = std::to_string(info.physical_cores) + " CPU cores: steady 40 fps framing.";
    return rec;
  }
  rec.preset_id = "low_spec_60";
  rec.carview_preset = "low";
  rec.label = "Low-Spec 60";
  rec.reason = gpu.discrete ? "Fast 60 fps preset: 1x scale, no MSAA, low shadows."
                            : "Integrated graphics: fast 60 fps preset (1x scale, no MSAA).";
  return rec;
}

std::string describe_hardware(const HardwareInfo& info) {
  std::ostringstream os;
  if (info.gpu) {
    os << info.gpu->name << " | " << format_bytes(info.gpu->vram_bytes)
       << (info.gpu->discrete ? " VRAM" : " Shared") << " | Vulkan " << info.gpu->vulkan_version_string();
  } else {
    os << "No Vulkan GPU detected";
  }
  if (info.physical_cores > 0) {
    os << " | " << info.physical_cores << "C/" << info.logical_threads << "T";
  } else if (info.logical_threads > 0) {
    os << " | " << info.logical_threads << " threads";
  }
  if (info.refresh_hz > 0) os << " | " << info.refresh_hz << " Hz";
  return os.str();
}

// ---- 4. Add to Steam Shortcut Generator (Binary VDF) -----------------------

uint32_t steam_shortcut_app_id(std::string_view quoted_exe, std::string_view app_name) {
  std::string key;
  key.reserve(quoted_exe.size() + app_name.size());
  key.append(quoted_exe);
  key.append(app_name);
  return crc32_string(key) | 0x80000000u;
}

namespace {

constexpr uint8_t kVdfMap = 0x00;
constexpr uint8_t kVdfString = 0x01;
constexpr uint8_t kVdfInt32 = 0x02;
constexpr uint8_t kVdfEnd = 0x08;

void vdf_put_cstr(std::vector<uint8_t>& out, std::string_view s) {
  out.insert(out.end(), s.begin(), s.end());
  out.push_back(0);
}
void vdf_put_string(std::vector<uint8_t>& out, std::string_view k, std::string_view v) {
  out.push_back(kVdfString);
  vdf_put_cstr(out, k);
  vdf_put_cstr(out, v);
}
void vdf_put_int32(std::vector<uint8_t>& out, std::string_view k, int32_t v) {
  out.push_back(kVdfInt32);
  vdf_put_cstr(out, k);
  write_le32(out, uint32_t(v));
}

bool vdf_read_cstr(std::span<const uint8_t> b, size_t& pos, std::string& out) {
  const size_t start = pos;
  while (pos < b.size() && b[pos] != 0) ++pos;
  if (pos >= b.size()) return false;
  out.assign(reinterpret_cast<const char*>(b.data() + start), pos - start);
  ++pos;
  return true;
}

bool vdf_skip_map(std::span<const uint8_t> b, size_t& pos) {
  while (pos < b.size()) {
    const uint8_t kind = b[pos++];
    if (kind == kVdfEnd) return true;
    std::string k;
    if (!vdf_read_cstr(b, pos, k)) return false;
    if (kind == kVdfMap) {
      if (!vdf_skip_map(b, pos)) return false;
    } else if (kind == kVdfString) {
      std::string v;
      if (!vdf_read_cstr(b, pos, v)) return false;
    } else if (kind == kVdfInt32) {
      if (pos + 4 > b.size()) return false;
      pos += 4;
    } else {
      return false;
    }
  }
  return false;
}

}  // namespace

std::vector<uint8_t> encode_shortcuts_vdf(const std::vector<SteamShortcutEntry>& entries) {
  std::vector<uint8_t> out;
  out.push_back(kVdfMap);
  vdf_put_cstr(out, "shortcuts");
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    out.push_back(kVdfMap);
    vdf_put_cstr(out, std::to_string(i));
    const uint32_t id = e.app_id != 0 ? e.app_id : steam_shortcut_app_id(e.exe, e.app_name);
    vdf_put_int32(out, "appid", int32_t(id));
    vdf_put_string(out, "AppName", e.app_name);
    vdf_put_string(out, "Exe", e.exe);
    vdf_put_string(out, "StartDir", e.start_dir);
    vdf_put_string(out, "icon", e.icon);
    vdf_put_string(out, "ShortcutPath", "");
    vdf_put_string(out, "LaunchOptions", e.launch_options);
    vdf_put_int32(out, "IsHidden", 0);
    vdf_put_int32(out, "AllowDesktopConfig", e.allow_desktop_config ? 1 : 0);
    vdf_put_int32(out, "AllowOverlay", e.allow_overlay ? 1 : 0);
    vdf_put_int32(out, "OpenVR", 0);
    vdf_put_int32(out, "Devkit", 0);
    vdf_put_string(out, "DevkitGameID", "");
    vdf_put_int32(out, "DevkitOverrideAppID", 0);
    vdf_put_int32(out, "LastPlayTime", 0);
    vdf_put_string(out, "FlatpakAppID", "");
    out.push_back(kVdfMap);
    vdf_put_cstr(out, "tags");
    out.push_back(kVdfEnd);
    out.push_back(kVdfEnd);
  }
  out.push_back(kVdfEnd);  // end shortcuts
  out.push_back(kVdfEnd);  // end root
  return out;
}

bool parse_shortcuts_vdf(std::span<const uint8_t> bytes, std::vector<SteamShortcutEntry>& out, std::string* error) {
  out.clear();
  if (bytes.empty()) return true;
  size_t pos = 0;
  while (pos < bytes.size()) {
    const uint8_t kind = bytes[pos++];
    if (kind == kVdfEnd) break;
    std::string key;
    if (!vdf_read_cstr(bytes, pos, key)) {
      if (error) *error = "truncated VDF root key";
      return false;
    }
    if (kind == kVdfMap && lower_ascii(key) == "shortcuts") {
      while (pos < bytes.size()) {
        const uint8_t ek = bytes[pos++];
        if (ek == kVdfEnd) break;
        std::string idx;
        if (!vdf_read_cstr(bytes, pos, idx) || ek != kVdfMap) {
          if (error) *error = "malformed shortcut map entry";
          return false;
        }
        SteamShortcutEntry entry;
        while (pos < bytes.size()) {
          const uint8_t fk = bytes[pos++];
          if (fk == kVdfEnd) break;
          std::string fk_name;
          if (!vdf_read_cstr(bytes, pos, fk_name)) return false;
          const std::string lk = lower_ascii(fk_name);
          if (fk == kVdfString) {
            std::string val;
            if (!vdf_read_cstr(bytes, pos, val)) return false;
            if (lk == "appname") entry.app_name = val;
            else if (lk == "exe") entry.exe = val;
            else if (lk == "startdir") entry.start_dir = val;
            else if (lk == "icon") entry.icon = val;
            else if (lk == "launchoptions") entry.launch_options = val;
          } else if (fk == kVdfInt32) {
            if (pos + 4 > bytes.size()) return false;
            const uint32_t v = read_le32(bytes.data() + pos);
            pos += 4;
            if (lk == "appid") entry.app_id = v;
            else if (lk == "allowdesktopconfig") entry.allow_desktop_config = v != 0;
            else if (lk == "allowoverlay") entry.allow_overlay = v != 0;
          } else if (fk == kVdfMap) {
            if (!vdf_skip_map(bytes, pos)) return false;
          } else {
            if (error) *error = "unsupported VDF field type";
            return false;
          }
        }
        out.push_back(std::move(entry));
      }
    } else if (kind == kVdfMap) {
      if (!vdf_skip_map(bytes, pos)) return false;
    } else if (kind == kVdfString) {
      std::string skip;
      if (!vdf_read_cstr(bytes, pos, skip)) return false;
    } else if (kind == kVdfInt32) {
      if (pos + 4 > bytes.size()) return false;
      pos += 4;
    } else {
      if (error) *error = "unsupported VDF root type";
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> upsert_steam_shortcut_vdf(std::span<const uint8_t> existing_vdf,
                                               const SteamShortcutEntry& entry, bool* updated_existing) {
  std::vector<SteamShortcutEntry> entries;
  if (!existing_vdf.empty()) {
    parse_shortcuts_vdf(existing_vdf, entries);
  }
  bool replaced = false;
  for (auto& e : entries) {
    if (lower_ascii(e.app_name) == lower_ascii(entry.app_name)) {
      e = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced) entries.push_back(entry);
  if (updated_existing) *updated_existing = replaced;
  return encode_shortcuts_vdf(entries);
}

SteamInstallResult add_to_steam(const LauncherPaths& paths, const std::string& app_name) {
  SteamInstallResult res;
  std::vector<std::filesystem::path> steam_roots;
  std::error_code ec;
#ifdef _WIN32
  if (const char* pf86 = std::getenv("ProgramFiles(x86)"); pf86 && *pf86)
    steam_roots.push_back(std::filesystem::path(pf86) / "Steam");
  if (const char* pf = std::getenv("ProgramFiles"); pf && *pf)
    steam_roots.push_back(std::filesystem::path(pf) / "Steam");
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home)
    steam_roots.push_back(std::filesystem::path(home) / "Library" / "Application Support" / "Steam");
#else
  if (const char* home = std::getenv("HOME"); home && *home) {
    steam_roots.push_back(std::filesystem::path(home) / ".local" / "share" / "Steam");
    steam_roots.push_back(std::filesystem::path(home) / ".steam" / "steam");
    steam_roots.push_back(std::filesystem::path(home) / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam");
  }
#endif

  const std::filesystem::path target_bin = !paths.carview_bin.empty() ? paths.carview_bin : paths.launcher_dir / "carview";
  SteamShortcutEntry entry;
  entry.app_name = app_name;
  entry.exe = "\"" + target_bin.string() + "\"";
  entry.start_dir = "\"" + paths.launcher_dir.string() + "\"";
  entry.launch_options = paths.portable ? ("--play \"" + paths.play_dir.string() + "\"") : "";
  entry.app_id = steam_shortcut_app_id(entry.exe, entry.app_name);

  std::set<std::filesystem::path> seen;
  for (const auto& root : steam_roots) {
    const auto userdata = root / "userdata";
    if (!std::filesystem::is_directory(userdata, ec)) continue;
    const auto canon = std::filesystem::weakly_canonical(userdata, ec);
    if (!seen.insert(canon).second) continue;
    for (const auto& user : std::filesystem::directory_iterator(userdata, ec)) {
      if (!user.is_directory(ec)) continue;
      const std::string uname = user.path().filename().string();
      if (uname == "0" || !std::all_of(uname.begin(), uname.end(), [](unsigned char c) { return std::isdigit(c); }))
        continue;
      const auto cfg = user.path() / "config";
      std::filesystem::create_directories(cfg, ec);
      const auto vdf_path = cfg / "shortcuts.vdf";
      std::vector<uint8_t> existing;
      if (std::filesystem::is_regular_file(vdf_path, ec)) {
        std::ifstream in(vdf_path, std::ios::binary);
        existing.assign(std::istreambuf_iterator<char>(in), {});
        const auto backup = cfg / "shortcuts.vdf.shift-backup";
        if (!std::filesystem::exists(backup, ec)) std::filesystem::copy_file(vdf_path, backup, ec);
      }
      const auto next_bytes = upsert_steam_shortcut_vdf(existing, entry);
      std::ofstream out(vdf_path, std::ios::binary | std::ios::trunc);
      if (out) {
        out.write(reinterpret_cast<const char*>(next_bytes.data()), std::streamsize(next_bytes.size()));
        ++res.steam_users_updated;
      }
    }
  }

#if !defined(_WIN32) && !defined(__APPLE__)
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  if ((xdg && *xdg) || (home && *home)) {
    const auto apps_dir = (xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home) / ".local" / "share") / "applications";
    std::filesystem::create_directories(apps_dir, ec);
    std::ofstream desk(apps_dir / "shift-engine.desktop");
    if (desk) {
      desk << "[Desktop Entry]\nType=Application\nName=" << app_name
           << "\nComment=Forza Horizon native engine\nExec=" << entry.exe
           << "\nCategories=Game;\nTerminal=false\n";
      res.desktop_entry_written = true;
    }
  }
#endif

  res.ok = res.steam_users_updated > 0 || res.desktop_entry_written;
  if (res.steam_users_updated > 0) {
    res.message = "Added " + app_name + " to Steam for " + std::to_string(res.steam_users_updated) +
                  " user(s). Restart Steam to see it under Non-Steam Games.";
  } else {
    res.message = "Steam installation not found for this account (checked default Steam userdata folders).";
  }
  return res;
}

// ---- 5. DLC, Title Update v4, UDLC v1.2 & Forza Horizon XE -----------------

bool is_stfs_package_file(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  char magic[4]{};
  in.read(magic, 4);
  if (in.gcount() != 4) return false;
  return std::memcmp(magic, "CON ", 4) == 0 || std::memcmp(magic, "LIVE", 4) == 0 ||
         std::memcmp(magic, "PIRS", 4) == 0;
}

ContentStatus scan_content_packages(const std::filesystem::path& state_root,
                                    const std::filesystem::path& mods_root) {
  ContentStatus st;
  std::error_code ec;

  // 1. Title Update v4
  ContentPackage v4;
  v4.id = "title-update-v4";
  v4.title = "Title Update v4 (Rally + 1000 Club)";
  v4.kind = ContentKind::kTitleUpdateV4;
  const std::filesystem::path v4_candidates[] = {
      state_root / "title-update-v4",
      state_root / "user" / "0000000000000000" / "4D5309C9" / "000B0000",
  };
  for (const auto& c : v4_candidates) {
    if (std::filesystem::is_directory(c, ec)) {
      v4.installed = true;
      v4.enabled = !std::filesystem::exists(state_root / "v4.disabled", ec);
      v4.verified = true;
      v4.path = c;
      v4.detail = "Installed under " + c.filename().string();
      break;
    }
  }
  st.has_v4 = v4.installed;
  st.v4_enabled = v4.enabled;
  st.packages.push_back(v4);

  // 2. Installed Xbox 360 DLC packages under 00000002 / dlc/
  const std::filesystem::path dlc_dirs[] = {
      state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002",
      state_root / "dlc",
  };
  std::set<std::string> seen_ids;
  for (const auto& d : dlc_dirs) {
    if (!std::filesystem::is_directory(d, ec)) continue;
    for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
      const std::string fname = e.path().filename().string();
      if (!seen_ids.insert(lower_ascii(fname)).second) continue;
      const bool stfs = e.is_regular_file(ec) && is_stfs_package_file(e.path());
      const bool dir_pkg = e.is_directory(ec);
      if (!stfs && !dir_pkg) continue;
      ContentPackage pkg;
      pkg.id = fname;
      pkg.title = fname;
      pkg.path = e.path();
      pkg.installed = true;
      pkg.enabled = true;
      pkg.verified = stfs || dir_pkg;
      if (e.is_regular_file(ec)) pkg.bytes = std::filesystem::file_size(e.path(), ec);
      const std::string lf = lower_ascii(fname);
      if (lf.find("rally") != std::string::npos) {
        pkg.kind = ContentKind::kRallyExpansion;
        pkg.title = "Horizon Rally Expansion Pack";
        st.has_rally = true;
      } else if (lf.find("1000") != std::string::npos || lf.find("club") != std::string::npos) {
        pkg.kind = ContentKind::kClub1000;
        pkg.title = "1000 Club Expansion Pack";
        st.has_1000_club = true;
      } else {
        pkg.kind = ContentKind::kDlcPack;
      }
      pkg.detail = stfs ? "Verified STFS container" : "Extracted offer package";
      ++st.dlc_installed_count;
      st.packages.push_back(std::move(pkg));
    }
  }

  // 3. Detect UDLC v1.2 & Forza Horizon XE under mods/ or state/
  ContentPackage udlc;
  udlc.id = "udlc-v1.2";
  udlc.title = "UDLC v1.2 Community DLC Pack";
  udlc.kind = ContentKind::kUdlcV12;
  ContentPackage xe;
  xe.id = "forza-horizon-xe";
  xe.title = "Forza Horizon XE";
  xe.kind = ContentKind::kHorizonXe;

  for (const auto& root : {mods_root, state_root}) {
    if (!std::filesystem::is_directory(root, ec)) continue;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
      if (!e.is_directory(ec)) continue;
      const std::string lf = lower_ascii(e.path().filename().string());
      if (lf.find("udlc") != std::string::npos) {
        udlc.installed = true;
        udlc.enabled = true;
        udlc.verified = true;
        udlc.path = e.path();
        udlc.detail = "Mounted from " + e.path().filename().string();
      }
      if (lf == "xe" || lf.find("horizon_xe") != std::string::npos || lf.find("horizon-xe") != std::string::npos ||
          lf.find("fh1_xe") != std::string::npos) {
        xe.installed = true;
        xe.enabled = true;
        xe.verified = true;
        xe.path = e.path();
        xe.detail = "Mounted from " + e.path().filename().string() + " (uses isolated save user-xe)";
      }
    }
  }
  st.has_udlc = udlc.installed;
  st.has_xe = xe.installed;
  st.packages.push_back(udlc);
  st.packages.push_back(xe);

  if (st.has_xe && st.v4_enabled) {
    st.notice = "Forza Horizon XE takes precedence over Title Update v4 and uses its own save (user-xe).";
  } else if (st.has_v4) {
    st.notice = "Title Update v4 detected (" + std::to_string(st.dlc_installed_count) + " DLC package(s)). Back up your base save before first v4 run.";
  } else {
    st.notice = std::to_string(st.dlc_installed_count) + " DLC package(s) installed. Import your own STFS/extracted DLC, v4, UDLC v1.2, or XE folders here.";
  }
  return st;
}

bool import_content_package(const std::filesystem::path& source_path,
                            const std::filesystem::path& state_root,
                            const std::filesystem::path& mods_root,
                            std::string& out_message) {
  std::error_code ec;
  if (!std::filesystem::exists(source_path, ec)) {
    out_message = "Source path does not exist: " + source_path.string();
    return false;
  }
  const std::string fname = source_path.filename().string();
  const std::string lf = lower_ascii(fname);

  if (std::filesystem::is_regular_file(source_path, ec)) {
    if (is_stfs_package_file(source_path)) {
      const bool is_tu = lf.rfind("tu", 0) == 0 || lf.find("update") != std::string::npos;
      const auto dest_dir = is_tu ? (state_root / "user" / "0000000000000000" / "4D5309C9" / "000B0000")
                                  : (state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002");
      std::filesystem::create_directories(dest_dir, ec);
      std::filesystem::copy_file(source_path, dest_dir / fname,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        out_message = "Failed to copy STFS package: " + ec.message();
        return false;
      }
      out_message = "Imported verified STFS package " + fname + " into " + dest_dir.string();
      return true;
    }
    out_message = "File is not a recognized STFS package (expected CON/LIVE/PIRS header) or folder.";
    return false;
  }

  // Folder import: route UDLC/XE into mods/, v4 into title-update-v4/, else dlc/<name>
  std::filesystem::path dest;
  if (lf.find("udlc") != std::string::npos || lf.find("xe") != std::string::npos) {
    dest = mods_root / fname;
  } else if (lf.find("v4") != std::string::npos || lf.find("title-update") != std::string::npos) {
    dest = state_root / "title-update-v4";
  } else {
    dest = state_root / "dlc" / fname;
  }
  std::filesystem::create_directories(dest, ec);
  std::filesystem::copy(source_path, dest,
                        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                        ec);
  if (ec) {
    out_message = "Failed to import folder: " + ec.message();
    return false;
  }
  out_message = "Imported package folder into " + dest.string();
  return true;
}

// ---- 6. Mod Manager (mod.toml + mod.txt compatible with Slice 13A) ---------

namespace {

std::vector<std::string> parse_toml_string_array(std::string_view raw) {
  std::vector<std::string> out;
  size_t pos = raw.find('[');
  if (pos == std::string_view::npos) return out;
  ++pos;
  while (pos < raw.size()) {
    while (pos < raw.size() && (std::isspace(static_cast<unsigned char>(raw[pos])) || raw[pos] == ',')) ++pos;
    if (pos >= raw.size() || raw[pos] == ']') break;
    if (raw[pos] == '"' || raw[pos] == '\'') {
      const char q = raw[pos++];
      const size_t s = pos;
      while (pos < raw.size() && raw[pos] != q) ++pos;
      out.emplace_back(raw.substr(s, pos - s));
      if (pos < raw.size()) ++pos;
    } else {
      const size_t s = pos;
      while (pos < raw.size() && raw[pos] != ',' && raw[pos] != ']') ++pos;
      out.push_back(trim(raw.substr(s, pos - s)));
    }
  }
  return out;
}

void parse_mod_toml(const std::filesystem::path& file, ModEntry& m) {
  std::ifstream in(file);
  if (!in) return;
  m.has_mod_toml = true;
  std::string section;
  ModOption* cur_opt = nullptr;
  for (std::string line; std::getline(in, line);) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      cur_opt = nullptr;
      if (section.rfind("options.", 0) == 0) {
        ModOption opt;
        opt.key = section.substr(8);
        opt.title = opt.key;
        m.options.push_back(std::move(opt));
        cur_opt = &m.options.back();
      }
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = lower_ascii(trim(line.substr(0, eq)));
    const std::string raw_v = trim(line.substr(eq + 1));
    const std::string v = strip_quotes(raw_v);

    if (cur_opt) {
      if (k == "title") cur_opt->title = v;
      else if (k == "description") cur_opt->description = v;
      else if (k == "value" || k == "default") cur_opt->value = v;
      else if (k == "choices") {
        for (const auto& c : parse_toml_string_array(raw_v)) {
          cur_opt->choices.push_back({c, c});
        }
      }
      continue;
    }

    if (k == "title" || (k == "name" && m.title == m.name)) m.title = v;
    else if (k == "version") m.version = v;
    else if (k == "author") m.author = v;
    else if (k == "description") m.description = v;
    else if (k == "url") m.url = v;
    else if (k == "profile") m.profile = v;
    else if (k == "enabled") m.enabled = (lower_ascii(v) != "false" && v != "0");
    else if (k == "priority") m.priority = std::atoi(v.c_str());
    else if (k == "shares_save") m.shares_save = (lower_ascii(v) == "true" || v == "1");
    else if (k == "requires") m.requires_mods = parse_toml_string_array(raw_v);
    else if (k == "conflicts") m.conflicts = parse_toml_string_array(raw_v);
  }
}

void parse_mod_txt(const std::filesystem::path& file, ModEntry& m) {
  std::ifstream in(file);
  if (!in) return;
  for (std::string line; std::getline(in, line);) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.rfind("title ", 0) == 0) m.title = trim(line.substr(6));
    else if (line.rfind("enabled ", 0) == 0) m.enabled = trim(line.substr(8)) != "0";
    else if (line.rfind("priority ", 0) == 0) m.priority = std::atoi(line.c_str() + 9);
    else if (line.rfind("author ", 0) == 0) m.author = trim(line.substr(7));
    else if (line.rfind("version ", 0) == 0) m.version = trim(line.substr(8));
    else if (line.rfind("description ", 0) == 0) m.description = trim(line.substr(12));
    else if (line.rfind("shares_save ", 0) == 0) m.shares_save = trim(line.substr(12)) != "0";
    else if (line.rfind("profile ", 0) == 0) m.profile = trim(line.substr(8));
  }
}

}  // namespace

std::vector<ModEntry> scan_mods(const std::filesystem::path& mods_root) {
  std::vector<ModEntry> out;
  std::error_code ec;
  if (mods_root.empty() || !std::filesystem::is_directory(mods_root, ec)) return out;

  for (const auto& e : std::filesystem::directory_iterator(mods_root, ec)) {
    if (!e.is_directory(ec)) continue;
    const std::string name = e.path().filename().string();
    if (name.empty() || name[0] == '_' || name[0] == '.') continue;
    const auto toml_p = e.path() / "mod.toml";
    const auto txt_p = e.path() / "mod.txt";
    const bool has_toml = std::filesystem::is_regular_file(toml_p, ec);
    const bool has_txt = std::filesystem::is_regular_file(txt_p, ec);
    if (!has_toml && !has_txt) continue;

    ModEntry m;
    m.name = name;
    m.title = name;
    m.dir = e.path();
    if (has_toml) parse_mod_toml(toml_p, m);
    if (has_txt) parse_mod_txt(txt_p, m);

    for (const char* preview : {"preview.png", "banner.png", "icon.png"}) {
      if (std::filesystem::is_regular_file(m.dir / preview, ec)) {
        m.preview_image = m.dir / preview;
        break;
      }
    }

    auto scan_sub = [&](const char* sub, const char* tag) {
      const auto d = m.dir / sub;
      if (!std::filesystem::is_directory(d, ec)) return;
      int n = 0;
      for (const auto& f : std::filesystem::recursive_directory_iterator(d, ec)) {
        if (f.is_regular_file(ec)) ++n;
      }
      if (n > 0) {
        m.file_count += n;
        m.contents.push_back(tag);
      }
    };
    scan_sub("bake", "cars/physics");
    scan_sub("sound", "engine audio");
    scan_sub("radio", "custom radio");
    scan_sub("media", "game assets");
    scan_sub("textures", "textures");
    scan_sub("shaders", "shaders");
    scan_sub("db", "database");
    scan_sub("sql", "database");
    if (std::filesystem::is_directory(m.dir / "db", ec) || std::filesystem::is_directory(m.dir / "sql", ec)) {
      m.has_database_patch = true;
      m.shares_save = false;
    }

    out.push_back(std::move(m));
  }

  std::sort(out.begin(), out.end(), [](const ModEntry& a, const ModEntry& b) {
    return a.priority != b.priority ? a.priority > b.priority : a.name < b.name;
  });

  // Check requires / conflicts among discovered mods
  std::map<std::string, bool> enabled_by_name;
  for (const auto& m : out) enabled_by_name[lower_ascii(m.name)] = m.enabled;
  for (auto& m : out) {
    for (const auto& req : m.requires_mods) {
      auto it = enabled_by_name.find(lower_ascii(req));
      if (it == enabled_by_name.end() || !it->second) {
        m.problems.push_back("Requires mod '" + req + "'");
      }
    }
    for (const auto& conf : m.conflicts) {
      auto it = enabled_by_name.find(lower_ascii(conf));
      if (it != enabled_by_name.end() && it->second) {
        m.problems.push_back("Conflicts with enabled mod '" + conf + "'");
      }
    }
  }
  return out;
}

ModSaveIsolation evaluate_save_isolation(const std::vector<ModEntry>& mods) {
  ModSaveIsolation iso;
  for (const auto& m : mods) {
    if (!m.enabled) continue;
    if (!m.profile.empty()) {
      iso.kind = "own-mod-save";
      iso.profile = m.profile;
      iso.owning_mod = m.title;
      iso.warning_banner = m.title + " uses its own isolated save (" + m.profile + "). Your main save stays untouched.";
      return iso;
    }
  }
  for (const auto& m : mods) {
    if (!m.enabled) continue;
    if (!m.shares_save || m.has_database_patch) {
      iso.kind = "modded";
      iso.profile = "user-modded";
      iso.owning_mod = m.title;
      iso.warning_banner = "Active database/gameplay mod (" + m.title + ") plays an isolated copy (user-modded). Your main save stays untouched.";
      return iso;
    }
  }
  iso.kind = "own";
  iso.profile = "user";
  iso.warning_banner = "All enabled mods are save-compatible and share your normal career save.";
  return iso;
}

bool write_mod_state(const ModEntry& mod, std::string* error) {
  std::ofstream out(mod.dir / "mod.txt", std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "Cannot write " + (mod.dir / "mod.txt").string();
    return false;
  }
  out << "title " << mod.title << "\n";
  out << "enabled " << (mod.enabled ? 1 : 0) << "\n";
  out << "priority " << mod.priority << "\n";
  if (!mod.version.empty()) out << "version " << mod.version << "\n";
  if (!mod.author.empty()) out << "author " << mod.author << "\n";
  if (!mod.description.empty()) out << "description " << mod.description << "\n";
  out << "shares_save " << (mod.shares_save ? 1 : 0) << "\n";
  if (!mod.profile.empty()) out << "profile " << mod.profile << "\n";

  if (mod.has_mod_toml && !mod.options.empty()) {
    std::ofstream opt_out(mod.dir / "mod-options.txt", std::ios::binary | std::ios::trunc);
    for (const auto& opt : mod.options) {
      opt_out << opt.key << " = " << opt.value << "\n";
    }
  }
  return bool(out);
}

bool toggle_mod_enabled(std::vector<ModEntry>& mods, size_t index, std::string* error) {
  if (index >= mods.size()) return false;
  mods[index].enabled = !mods[index].enabled;
  return write_mod_state(mods[index], error);
}

bool move_mod_order(std::vector<ModEntry>& mods, size_t index, int delta, std::string* error) {
  if (mods.size() < 2 || index >= mods.size()) return false;
  const int next_i = int(index) + delta;
  if (next_i < 0 || next_i >= int(mods.size())) return false;
  std::swap(mods[index], mods[size_t(next_i)]);
  // Re-assign monotonically decreasing priorities so winner stays first on reload.
  const int base = int(mods.size()) * 10;
  for (size_t i = 0; i < mods.size(); ++i) {
    mods[i].priority = base - int(i) * 10;
    if (!write_mod_state(mods[i], error)) return false;
  }
  return true;
}

bool set_mod_option_value(ModEntry& mod, const std::string& key, const std::string& value, std::string* error) {
  for (auto& opt : mod.options) {
    if (opt.key == key) {
      opt.value = value;
      return write_mod_state(mod, error);
    }
  }
  if (error) *error = "Unknown mod option: " + key;
  return false;
}

bool install_mod_source(const std::filesystem::path& source_folder_or_zip,
                        const std::filesystem::path& mods_root,
                        std::string& out_message) {
  std::error_code ec;
  if (!std::filesystem::exists(source_folder_or_zip, ec)) {
    out_message = "Mod source not found: " + source_folder_or_zip.string();
    return false;
  }
  std::filesystem::create_directories(mods_root, ec);

  if (std::filesystem::is_directory(source_folder_or_zip, ec)) {
    const std::string name = source_folder_or_zip.filename().string();
    const auto dest = mods_root / name;
    std::filesystem::copy(source_folder_or_zip, dest,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
      out_message = "Failed to copy mod folder: " + ec.message();
      return false;
    }
    if (!std::filesystem::exists(dest / "mod.txt", ec) && !std::filesystem::exists(dest / "mod.toml", ec)) {
      std::ofstream f(dest / "mod.txt");
      f << "title " << name << "\nenabled 1\npriority 0\n";
    }
    out_message = "Installed mod '" + name + "' into " + dest.string();
    return true;
  }

  if (lower_ascii(source_folder_or_zip.extension().string()) == ".zip") {
    const std::string stem = source_folder_or_zip.stem().string();
    const auto dest = mods_root / stem;
    std::filesystem::create_directories(dest, ec);
#ifdef _WIN32
    const std::string cmd = "powershell -NoProfile -Command \"Expand-Archive -Force -LiteralPath '" +
                            source_folder_or_zip.string() + "' -DestinationPath '" + dest.string() + "'\"";
#else
    const std::string cmd = "unzip -oq " + quote_arg_shell(source_folder_or_zip.string()) + " -d " +
                            quote_arg_shell(dest.string());
#endif
    if (std::system(cmd.c_str()) != 0) {
      out_message = "Failed to extract mod ZIP: " + source_folder_or_zip.string();
      return false;
    }
    if (!std::filesystem::exists(dest / "mod.txt", ec) && !std::filesystem::exists(dest / "mod.toml", ec)) {
      std::ofstream f(dest / "mod.txt");
      f << "title " << stem << "\nenabled 1\npriority 0\n";
    }
    out_message = "Extracted mod ZIP '" + stem + "' into " + dest.string();
    return true;
  }

  out_message = "Select a mod folder or .zip archive.";
  return false;
}

// ---- 7. Custom Radio Station Builder ---------------------------------------

std::vector<RadioTrackInfo> scan_radio_music_folder(const std::filesystem::path& folder) {
  static const std::set<std::string> kExts = {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac", ".opus", ".wma"};
  std::vector<RadioTrackInfo> out;
  std::error_code ec;
  if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return out;
  for (const auto& e : std::filesystem::recursive_directory_iterator(folder, ec)) {
    if (!e.is_regular_file(ec)) continue;
    if (!kExts.count(lower_ascii(e.path().extension().string()))) continue;
    RadioTrackInfo t;
    t.path = e.path();
    t.bytes = std::filesystem::file_size(e.path(), ec);
    const std::string stem = e.path().stem().string();
    const size_t dash = stem.find(" - ");
    if (dash != std::string::npos) {
      t.artist = trim(stem.substr(0, dash));
      t.title = trim(stem.substr(dash + 3));
    } else {
      t.title = stem;
    }
    out.push_back(std::move(t));
  }
  std::sort(out.begin(), out.end(), [](const RadioTrackInfo& a, const RadioTrackInfo& b) {
    return lower_ascii(a.path.string()) < lower_ascii(b.path.string());
  });
  return out;
}

LogoValidation validate_station_logo_png(const std::filesystem::path& png_path) {
  LogoValidation v;
  if (png_path.empty()) {
    v.message = "Optional: select a 210x100 PNG station logo.";
    return v;
  }
  std::ifstream in(png_path, std::ios::binary);
  if (!in) {
    v.message = "Cannot open logo file.";
    return v;
  }
  uint8_t hdr[24]{};
  in.read(reinterpret_cast<char*>(hdr), 24);
  if (in.gcount() != 24) {
    v.message = "File is too small to be a PNG.";
    return v;
  }
  static constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(hdr, kPngSig, 8) != 0 || std::memcmp(hdr + 12, "IHDR", 4) != 0) {
    v.message = "Logo must be a valid PNG image.";
    return v;
  }
  v.width = read_be32(hdr + 16);
  v.height = read_be32(hdr + 20);
  if (v.width == 210 && v.height == 100) {
    v.valid = true;
    v.message = "Valid 210x100 PNG station logo.";
  } else {
    v.valid = false;
    v.message = "PNG is " + std::to_string(v.width) + "x" + std::to_string(v.height) + " (expected 210x100).";
  }
  return v;
}

std::string merge_radio_system_xml(const std::string& xml, const std::string& station_slot,
                                   const std::vector<std::tuple<std::string, std::string, std::string>>& tracks,
                                   bool replace, std::string* error) {
  const std::string marker = "name=\"" + station_slot + "\"";
  const size_t st_pos = xml.find(marker);
  if (st_pos == std::string::npos) {
    if (error) *error = "RadioSystem.xml has no station " + station_slot;
    return {};
  }
  const size_t st_end = xml.find("</RadioStation>", st_pos);
  const size_t pl_open = xml.find("<Playlist", st_pos);
  if (pl_open == std::string::npos || (st_end != std::string::npos && pl_open > st_end)) {
    if (error) *error = station_slot + " has no <Playlist>";
    return {};
  }
  const size_t pl_open_end = xml.find('>', pl_open);
  const size_t pl_close = xml.find("</Playlist>", pl_open_end);
  if (pl_open_end == std::string::npos || pl_close == std::string::npos) {
    if (error) *error = "Malformed <Playlist> tag";
    return {};
  }

  std::string opening = xml.substr(pl_open, pl_open_end - pl_open + 1);
  if (replace) {
    const size_t nr = opening.find("noRepeat=\"");
    if (nr != std::string::npos) {
      const size_t nr_end = opening.find('"', nr + 10);
      if (nr_end != std::string::npos) {
        const int cap = std::max(0, std::min(15, int(tracks.size()) - 1));
        opening = opening.substr(0, nr + 10) + std::to_string(cap) + opening.substr(nr_end);
      }
    }
  }
  std::string body = replace ? "" : xml.substr(pl_open_end + 1, pl_close - (pl_open_end + 1));
  while (!body.empty() && (body.back() == ' ' || body.back() == '\t' || body.back() == '\r' || body.back() == '\n'))
    body.pop_back();

  std::ostringstream lines;
  for (const auto& [title, artist, fmod_name] : tracks) {
    lines << "\n\t\t\t<MusicTrack name=\"" << xml_escape(title) << "\" artist=\"" << xml_escape(artist)
          << "\" fmodname=\"" << xml_escape(fmod_name)
          << "\" likelihoodDaytime=\"1\" likelihoodMorning=\"1\" likelihoodEvening=\"1\" likelihoodNight=\"1\" />";
  }
  lines << "\n\t\t";
  return xml.substr(0, pl_open) + opening + body + lines.str() + xml.substr(pl_close);
}

std::string merge_radio_soundbank_xml(const std::string& xml,
                                      const std::vector<std::tuple<std::string, int, uint32_t, uint32_t>>& entries,
                                      std::string* error) {
  const size_t end = xml.rfind("</SoundBanks>");
  if (end == std::string::npos) {
    if (error) *error = "RadioSoundbankInfo_Music.xml has no </SoundBanks>";
    return {};
  }
  const std::string nl = xml.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::ostringstream os;
  for (const auto& [name, idx, ident_ms, len_ms] : entries) {
    os << "  <Bank Name=\"" << xml_escape(name) << "\">" << nl
       << "    <SoundBankInfo SoundBankIndex=\"" << idx << "\" OffsetEventStart=\"0\" OffsetIdentStart=\""
       << ident_ms << "\" OffsetSongStart=\"0\" OffsetStartNextTrack=\"0\" FileLength=\"" << len_ms
       << "\" SyncpointIndexIdentStart=\"2\" SyncpointStartNextTrack=\"-1\"/>" << nl
       << "  </Bank>" << nl;
  }
  return xml.substr(0, end) + os.str() + xml.substr(end);
}

namespace {

struct Fsb4Sample {
  std::string name;
  uint32_t length_samples = 0;
  uint32_t length_bytes = 0;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
  uint32_t mode = 0x90000200u;
  int32_t frequency = 48000;
  uint16_t volume = 255;
  int16_t pan = 128;
  uint16_t priority = 128;
  uint16_t channels = 2;
  std::array<uint8_t, 16> tail{};
  std::vector<std::pair<uint32_t, std::string>> syncs;
  uint64_t data_offset = 0;
};

constexpr uint32_t kFsbAlign = 32;
uint32_t fsb_aligned(uint32_t sz) { return (sz + kFsbAlign - 1u) & ~(kFsbAlign - 1u); }

void serialize_fsb4_sample_header(const Fsb4Sample& s, std::vector<uint8_t>& out) {
  std::vector<uint8_t> extra;
  if (!s.syncs.empty()) {
    extra.insert(extra.end(), {'S', 'Y', 'N', 'C'});
    write_le32(extra, uint32_t(s.syncs.size()));
    for (const auto& [off, lbl] : s.syncs) {
      write_le32(extra, off);
      size_t start = extra.size();
      extra.resize(start + 256, 0);
      std::memcpy(extra.data() + start, lbl.data(), std::min<size_t>(lbl.size(), 255));
    }
  }
  const uint16_t size = uint16_t(80 + extra.size());
  write_le16(out, size);
  const size_t nstart = out.size();
  out.resize(nstart + 30, 0);
  std::memcpy(out.data() + nstart, s.name.data(), std::min<size_t>(s.name.size(), 29));
  write_le32(out, s.length_samples);
  write_le32(out, s.length_bytes);
  write_le32(out, s.loop_start);
  write_le32(out, s.loop_end);
  write_le32(out, s.mode);
  write_le32(out, uint32_t(s.frequency));
  write_le16(out, s.volume);
  write_le16(out, uint16_t(s.pan));
  write_le16(out, s.priority);
  write_le16(out, s.channels);
  out.insert(out.end(), s.tail.begin(), s.tail.end());
  out.insert(out.end(), extra.begin(), extra.end());
}

}  // namespace

RadioBuildResult build_custom_radio_mod(const RadioBuilderConfig& cfg,
                                        const std::filesystem::path& game_root,
                                        const std::filesystem::path& mods_root,
                                        const std::filesystem::path& ffmpeg_bin,
                                        const std::function<void(const std::string&)>& log_cb) {
  RadioBuildResult res;
  auto log = [&](const std::string& s) {
    if (log_cb) log_cb(s);
  };

  const auto tracks = scan_radio_music_folder(cfg.music_folder);
  if (tracks.empty()) {
    res.message = "No supported audio files (MP3/WAV/FLAC/OGG/M4A) found in music folder.";
    return res;
  }
  if (!cfg.logo_png_path.empty()) {
    const auto lv = validate_station_logo_png(cfg.logo_png_path);
    if (!lv.valid) {
      res.message = "Station logo error: " + lv.message;
      return res;
    }
  }
  const auto ff = ffmpeg_bin.empty() ? find_in_path("ffmpeg") : ffmpeg_bin;
  if (ff.empty()) {
    res.message = "FFmpeg is required to encode custom radio tracks (48 kHz 160 kbps MP3).";
    return res;
  }

  const auto radio_src = game_root / "media" / "audio" / "radio";
  const auto stock_fsb = radio_src / "Radio_Music.fsb";
  std::ifstream fsb_in(stock_fsb, std::ios::binary);
  if (!fsb_in) {
    res.message = "Cannot open stock radio bank: " + stock_fsb.string();
    return res;
  }

  uint8_t head[48]{};
  fsb_in.read(reinterpret_cast<char*>(head), 48);
  if (fsb_in.gcount() != 48 || std::memcmp(head, "FSB4", 4) != 0) {
    res.message = "Stock Radio_Music.fsb is not a valid FSB4 bank.";
    return res;
  }
  const uint32_t sample_count = read_le32(head + 4);
  const uint32_t header_size = read_le32(head + 8);
  const uint32_t version = read_le32(head + 16);
  const uint32_t bank_mode = read_le32(head + 20);
  std::vector<uint8_t> raw_hdr(header_size);
  fsb_in.read(reinterpret_cast<char*>(raw_hdr.data()), std::streamsize(header_size));
  if (size_t(fsb_in.gcount()) != header_size) {
    res.message = "Truncated FSB4 header in stock Radio_Music.fsb.";
    return res;
  }

  std::vector<Fsb4Sample> samples;
  samples.reserve(sample_count + tracks.size());
  size_t off = 0;
  uint64_t pos = 48u + header_size;
  std::set<std::string> taken;
  for (uint32_t i = 0; i < sample_count && off + 80 <= raw_hdr.size(); ++i) {
    const uint16_t sz = read_le16(raw_hdr.data() + off);
    if (sz < 80 || off + sz > raw_hdr.size()) break;
    Fsb4Sample s;
    const char* nptr = reinterpret_cast<const char*>(raw_hdr.data() + off + 2);
    s.name = std::string(nptr, ::strnlen(nptr, 30));
    s.length_samples = read_le32(raw_hdr.data() + off + 32);
    s.length_bytes = read_le32(raw_hdr.data() + off + 36);
    s.loop_start = read_le32(raw_hdr.data() + off + 40);
    s.loop_end = read_le32(raw_hdr.data() + off + 44);
    s.mode = read_le32(raw_hdr.data() + off + 48);
    s.frequency = int32_t(read_le32(raw_hdr.data() + off + 52));
    s.volume = read_le16(raw_hdr.data() + off + 56);
    s.pan = int16_t(read_le16(raw_hdr.data() + off + 58));
    s.priority = read_le16(raw_hdr.data() + off + 60);
    s.channels = read_le16(raw_hdr.data() + off + 62);
    std::memcpy(s.tail.data(), raw_hdr.data() + off + 64, 16);
    if (sz > 88 && std::memcmp(raw_hdr.data() + off + 80, "SYNC", 4) == 0) {
      const uint32_t n = read_le32(raw_hdr.data() + off + 84);
      for (uint32_t k = 0; k < n && off + 88 + (k + 1) * 260 <= off + sz; ++k) {
        const uint8_t* ep = raw_hdr.data() + off + 88 + k * 260;
        const uint32_t soff = read_le32(ep);
        const char* slbl = reinterpret_cast<const char*>(ep + 4);
        s.syncs.emplace_back(soff, std::string(slbl, ::strnlen(slbl, 256)));
      }
    }
    s.data_offset = pos;
    pos += fsb_aligned(s.length_bytes);
    taken.insert(lower_ascii(s.name));
    samples.push_back(std::move(s));
    off += sz;
  }

  Fsb4Sample tmpl = samples.empty() ? Fsb4Sample{} : samples.front();
  for (const auto& s : samples) {
    if (s.channels == 2) {
      tmpl = s;
      break;
    }
  }

  std::error_code ec;
  const std::string mod_name = cfg.mod_name.empty() ? "custom_radio" : cfg.mod_name;
  const auto mod_dir = mods_root / mod_name;
  const auto out_radio = mod_dir / "radio";
  const auto temp_dir = mod_dir / ".tmp_encode";
  std::filesystem::create_directories(out_radio, ec);
  std::filesystem::create_directories(temp_dir, ec);

  std::vector<Fsb4Sample> added;
  std::vector<std::filesystem::path> encoded_files;
  std::vector<std::tuple<std::string, std::string, std::string>> xml_tracks;
  std::vector<std::tuple<std::string, int, uint32_t, uint32_t>> bank_xml_entries;

  for (size_t i = 0; i < tracks.size(); ++i) {
    const auto& tr = tracks[i];
    std::string slug;
    for (char c : tr.title) {
      if (std::isalnum(static_cast<unsigned char>(c)) && slug.size() < 18) slug += c;
    }
    if (slug.empty()) slug = "Track";
    char idbuf[32];
    std::snprintf(idbuf, sizeof idbuf, "SE%03zu_%s", i + 1, slug.c_str());
    std::string sample_id(idbuf);
    while (taken.count(lower_ascii(sample_id))) sample_id += "_";
    taken.insert(lower_ascii(sample_id));

    const auto mp3_out = temp_dir / (sample_id + ".mp3");
    const std::string cmd = quote_arg_shell(ff.string()) +
                            " -v error -y -nostdin -i " + quote_arg_shell(tr.path.string()) +
                            " -vn -map_metadata -1 -ar 48000 -ac 2 -c:a libmp3lame -b:a 160k"
                            " -write_xing 0 -id3v2_version 0 -write_id3v1 0 -f mp3 " +
                            quote_arg_shell(mp3_out.string());
    log("Encoding " + tr.title + " -> " + sample_id);
    if (std::system(cmd.c_str()) != 0 || !std::filesystem::is_regular_file(mp3_out, ec)) {
      std::filesystem::remove_all(temp_dir, ec);
      res.message = "FFmpeg failed to encode: " + tr.path.filename().string();
      return res;
    }
    const uint64_t fbytes = std::filesystem::file_size(mp3_out, ec);
    const uint32_t frames = uint32_t(fbytes / 480u);
    if (frames == 0) continue;

    Fsb4Sample ns = tmpl;
    ns.name = sample_id;
    ns.length_samples = frames * 1152u;
    ns.length_bytes = frames * 480u;
    ns.loop_start = 0;
    ns.loop_end = ns.length_samples - 1u;
    ns.mode = 0x90000200u;
    ns.frequency = 48000;
    ns.channels = 2;
    const uint32_t ident_samp = ns.length_samples > 192000u ? (ns.length_samples - 192000u) : 0u;
    ns.syncs = {{0u, "SongStart"}, {0u, "EventStart"}, {ident_samp, "IdentStart"}};

    const int bank_idx = int(samples.size() + added.size());
    bank_xml_entries.emplace_back(sample_id, bank_idx, uint32_t((uint64_t(ident_samp) * 1000u) / 48000u),
                                  uint32_t((uint64_t(ns.length_samples) * 1000u) / 48000u));
    xml_tracks.emplace_back(tr.title, tr.artist, sample_id);
    added.push_back(std::move(ns));
    encoded_files.push_back(mp3_out);
  }

  // Write merged FSB4 bank
  std::vector<Fsb4Sample> all = samples;
  all.insert(all.end(), added.begin(), added.end());
  std::vector<uint8_t> hdr_bytes;
  for (const auto& s : all) serialize_fsb4_sample_header(s, hdr_bytes);
  while ((48u + hdr_bytes.size()) % kFsbAlign != 0) hdr_bytes.push_back(0);
  uint32_t total_data = 0;
  for (const auto& s : all) total_data += fsb_aligned(s.length_bytes);

  std::ofstream fsb_out(out_radio / "Radio_Music.fsb", std::ios::binary | std::ios::trunc);
  if (!fsb_out) {
    std::filesystem::remove_all(temp_dir, ec);
    res.message = "Cannot write output Radio_Music.fsb";
    return res;
  }
  std::vector<uint8_t> file_hdr;
  file_hdr.insert(file_hdr.end(), {'F', 'S', 'B', '4'});
  write_le32(file_hdr, uint32_t(all.size()));
  write_le32(file_hdr, uint32_t(hdr_bytes.size()));
  write_le32(file_hdr, total_data);
  write_le32(file_hdr, version);
  write_le32(file_hdr, bank_mode);
  file_hdr.insert(file_hdr.end(), head + 24, head + 48);
  fsb_out.write(reinterpret_cast<const char*>(file_hdr.data()), std::streamsize(file_hdr.size()));
  fsb_out.write(reinterpret_cast<const char*>(hdr_bytes.data()), std::streamsize(hdr_bytes.size()));

  // Copy stock audio payload
  fsb_in.seekg(std::streamoff(48u + header_size), std::ios::beg);
  std::vector<char> copy_buf(1 << 20);
  while (fsb_in) {
    fsb_in.read(copy_buf.data(), std::streamsize(copy_buf.size()));
    const auto g = fsb_in.gcount();
    if (g > 0) fsb_out.write(copy_buf.data(), g);
  }
  // Append newly encoded tracks with 32-byte zero alignment
  std::vector<char> zeros(kFsbAlign, 0);
  for (size_t i = 0; i < added.size(); ++i) {
    std::ifstream mp3_in(encoded_files[i], std::ios::binary);
    std::vector<char> data(added[i].length_bytes);
    mp3_in.read(data.data(), std::streamsize(data.size()));
    fsb_out.write(data.data(), std::streamsize(data.size()));
    const uint32_t pad = fsb_aligned(added[i].length_bytes) - added[i].length_bytes;
    if (pad > 0) fsb_out.write(zeros.data(), std::streamsize(pad));
  }
  fsb_out.close();
  std::filesystem::remove_all(temp_dir, ec);

  // Merge RadioSoundbankInfo_Music.xml and RadioSystem.xml
  auto read_text = [](const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  std::string xerr;
  const std::string info_xml = read_text(radio_src / "RadioSoundbankInfo_Music.xml");
  const std::string merged_info = merge_radio_soundbank_xml(info_xml, bank_xml_entries, &xerr);
  if (!merged_info.empty()) {
    std::ofstream f(out_radio / "RadioSoundbankInfo_Music.xml", std::ios::binary | std::ios::trunc);
    f.write(merged_info.data(), std::streamsize(merged_info.size()));
  }
  const std::string sys_xml = read_text(radio_src / "RadioSystem.xml");
  const std::string merged_sys = merge_radio_system_xml(sys_xml, cfg.station_slot, xml_tracks, cfg.replace_playlist, &xerr);
  if (!merged_sys.empty()) {
    std::ofstream f(out_radio / "RadioSystem.xml", std::ios::binary | std::ios::trunc);
    f.write(merged_sys.data(), std::streamsize(merged_sys.size()));
  }

  // Stage custom logo & station name string override
  if (!cfg.logo_png_path.empty()) {
    const auto logo_dir = mod_dir / "members" / "ui" / "radio";
    std::filesystem::create_directories(logo_dir, ec);
    std::filesystem::copy_file(cfg.logo_png_path, logo_dir / (cfg.station_slot + "_logo.png"),
                               std::filesystem::copy_options::overwrite_existing, ec);
  }
  if (!cfg.custom_station_name.empty()) {
    const auto str_dir = mod_dir / "members" / "ui" / "radio";
    std::filesystem::create_directories(str_dir, ec);
    std::ofstream sf(str_dir / "station_names.txt", std::ios::binary | std::ios::trunc);
    sf << cfg.station_slot << "=" << cfg.custom_station_name << "\n";
  }

  // Write both mod.toml (Slice 13A) and mod.txt (carview loader)
  {
    std::ofstream tf(mod_dir / "mod.toml", std::ios::binary | std::ios::trunc);
    tf << "name = \"" << mod_name << "\"\n"
       << "title = \"" << (cfg.custom_station_name.empty() ? mod_name : cfg.custom_station_name) << "\"\n"
       << "version = \"1.0.0\"\n"
       << "author = \"Shift Launcher Radio Builder\"\n"
       << "description = \"Custom " << cfg.station_slot << " playlist (" << added.size() << " tracks)\"\n"
       << "shares_save = true\n";
    std::ofstream mf(mod_dir / "mod.txt", std::ios::binary | std::ios::trunc);
    mf << "title " << (cfg.custom_station_name.empty() ? mod_name : cfg.custom_station_name) << "\n"
       << "enabled 1\n"
       << "priority 50\n"
       << "shares_save 1\n";
  }

  res.ok = true;
  res.tracks_encoded = int(added.size());
  res.mod_dir = mod_dir;
  res.message = "Built custom radio mod '" + mod_name + "' (" + std::to_string(res.tracks_encoded) +
                " tracks on " + cfg.station_slot + ").";
  return res;
}

// ---- 8. Wheel & Controller Configurator ------------------------------------

WheelBindingConfig load_wheel_settings(const std::filesystem::path& settings_file) {
  WheelBindingConfig cfg;
  std::ifstream in(settings_file);
  if (!in) return cfg;
  for (std::string line; std::getline(in, line);) {
    line = trim(line);
    if (line.empty()) continue;
    const bool is_comment_cfg = line.rfind("# wheel-", 0) == 0;
    if (line[0] == '#' && !is_comment_cfg) continue;
    if (is_comment_cfg) line = trim(line.substr(2));
    const size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    const std::string k = line.substr(0, sp);
    const std::string v = trim(line.substr(sp + 1));
    if (k == "steer-dz-in") cfg.steer.deadzone_in = float(std::atof(v.c_str()));
    else if (k == "steer-dz-out") cfg.steer.deadzone_out = float(std::atof(v.c_str()));
    else if (k == "throttle-dz-in") cfg.throttle.deadzone_in = float(std::atof(v.c_str()));
    else if (k == "throttle-dz-out") cfg.throttle.deadzone_out = float(std::atof(v.c_str()));
    else if (k == "brake-dz-in") cfg.brake.deadzone_in = float(std::atof(v.c_str()));
    else if (k == "brake-dz-out") cfg.brake.deadzone_out = float(std::atof(v.c_str()));
    else if (k == "rumble-scale") cfg.rumble_scale = float(std::atof(v.c_str()));
    else if (k == "controller-layout") cfg.controller_layout = std::atoi(v.c_str());
    else if (k == "wheel-separate-devices") cfg.separate_devices = (v == "1" || v == "true");
    else if (k == "wheel-degrees") cfg.steering_degrees = std::atoi(v.c_str());
    else if (k == "wheel-ffb-gain") cfg.ffb_gain = float(std::atof(v.c_str()));
    else if (k == "wheel-ffb-invert") cfg.ffb_invert = (v == "1" || v == "true");
    else if (k == "wheel-steer-axis") cfg.steer.axis_index = std::atoi(v.c_str());
    else if (k == "wheel-throttle-axis") cfg.throttle.axis_index = std::atoi(v.c_str());
    else if (k == "wheel-brake-axis") cfg.brake.axis_index = std::atoi(v.c_str());
    else if (k == "wheel-clutch-axis") cfg.clutch.axis_index = std::atoi(v.c_str());
    else if (k == "wheel-handbrake-axis") cfg.handbrake.axis_index = std::atoi(v.c_str());
  }
  return cfg;
}

bool save_wheel_settings(const std::filesystem::path& settings_file, const WheelBindingConfig& cfg,
                         std::string* error) {
  static const std::set<std::string> kManagedKeys = {
      "steer-dz-in", "steer-dz-out", "throttle-dz-in", "throttle-dz-out",
      "brake-dz-in", "brake-dz-out", "rumble-scale", "controller-layout"};
  std::vector<std::string> kept_lines;
  if (std::ifstream in(settings_file); in) {
    for (std::string line; std::getline(in, line);) {
      const std::string t = trim(line);
      if (t.rfind("# wheel-", 0) == 0) continue;
      if (!t.empty() && t[0] != '#') {
        const size_t sp = t.find(' ');
        const std::string k = sp == std::string::npos ? t : t.substr(0, sp);
        if (kManagedKeys.count(k)) continue;
      }
      kept_lines.push_back(line);
    }
  }
  std::error_code ec;
  if (settings_file.has_parent_path()) std::filesystem::create_directories(settings_file.parent_path(), ec);
  std::ofstream out(settings_file, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "Cannot write " + settings_file.string();
    return false;
  }
  if (kept_lines.empty()) {
    out << "# shift-engine settings: one option per line, the command line wins (carview --help)\n";
  } else {
    for (const auto& l : kept_lines) out << l << "\n";
  }
  auto fmt_f = [](float f) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", double(f));
    return std::string(b);
  };
  out << "controller-layout " << cfg.controller_layout << "\n";
  out << "steer-dz-in " << fmt_f(cfg.steer.deadzone_in) << "\n";
  out << "steer-dz-out " << fmt_f(cfg.steer.deadzone_out) << "\n";
  out << "throttle-dz-in " << fmt_f(cfg.throttle.deadzone_in) << "\n";
  out << "throttle-dz-out " << fmt_f(cfg.throttle.deadzone_out) << "\n";
  out << "brake-dz-in " << fmt_f(cfg.brake.deadzone_in) << "\n";
  out << "brake-dz-out " << fmt_f(cfg.brake.deadzone_out) << "\n";
  out << "rumble-scale " << fmt_f(cfg.rumble_scale) << "\n";
  // Store wheel-specific metadata as structured comment directives so carview's strict settings.txt parser never warns.
  out << "# wheel-separate-devices " << (cfg.separate_devices ? 1 : 0) << "\n";
  out << "# wheel-degrees " << cfg.steering_degrees << "\n";
  out << "# wheel-ffb-gain " << fmt_f(cfg.ffb_gain) << "\n";
  out << "# wheel-ffb-invert " << (cfg.ffb_invert ? 1 : 0) << "\n";
  out << "# wheel-steer-axis " << cfg.steer.axis_index << "\n";
  out << "# wheel-throttle-axis " << cfg.throttle.axis_index << "\n";
  out << "# wheel-brake-axis " << cfg.brake.axis_index << "\n";
  out << "# wheel-clutch-axis " << cfg.clutch.axis_index << "\n";
  out << "# wheel-handbrake-axis " << cfg.handbrake.axis_index << "\n";
  return bool(out);
}

// ---- 9. Self-Updater -------------------------------------------------------

std::string ReleaseVersion::to_string() const {
  std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  if (!prerelease.empty()) s += "-" + prerelease;
  return s;
}

bool parse_release_version(std::string_view text, ReleaseVersion& out) {
  std::string s = trim(text);
  if (s.empty()) return false;
  if (s[0] == 'v' || s[0] == 'V') s = s.substr(1);
  if (const size_t plus = s.find('+'); plus != std::string::npos) s = s.substr(0, plus);
  std::string pre;
  if (const size_t dash = s.find('-'); dash != std::string::npos) {
    pre = s.substr(dash + 1);
    s = s.substr(0, dash);
    if (pre.empty()) return false;
  }
  std::vector<int> nums;
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t dot = s.find('.', pos);
    const std::string part = s.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
    if (part.empty() || !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); }))
      return false;
    nums.push_back(std::atoi(part.c_str()));
    if (dot == std::string::npos) break;
    pos = dot + 1;
  }
  if (nums.size() != 3) return false;
  out.major = nums[0];
  out.minor = nums[1];
  out.patch = nums[2];
  out.prerelease = pre;
  return true;
}

int compare_release_version(const ReleaseVersion& a, const ReleaseVersion& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.prerelease.empty() && b.prerelease.empty()) return 0;
  if (a.prerelease.empty()) return 1;   // stable > prerelease
  if (b.prerelease.empty()) return -1;
  return a.prerelease < b.prerelease ? -1 : (a.prerelease > b.prerelease ? 1 : 0);
}

std::optional<AvailableUpdate> select_github_update(std::string_view releases_json,
                                                    std::string_view current_version,
                                                    std::string_view channel,
                                                    std::string_view platform_asset_name) {
  ReleaseVersion installed;
  if (!parse_release_version(current_version, installed)) return std::nullopt;
  JsonVal root;
  JsonParser parser(releases_json);
  if (!parser.parse(root) || root.kind != JsonVal::kArray) return std::nullopt;

  const bool allow_preview = lower_ascii(channel) == "preview";
  std::optional<AvailableUpdate> best;
  ReleaseVersion best_ver;
  bool have_best = false;

  for (const auto& rel : root.arr) {
    if (rel.kind != JsonVal::kObject) continue;
    if (const auto* d = rel.get("draft"); d && d->as_bool()) continue;
    if (!allow_preview) {
      if (const auto* pre = rel.get("prerelease"); pre && pre->as_bool()) continue;
    }
    const auto* tag_v = rel.get("tag_name");
    if (!tag_v) continue;
    ReleaseVersion ver;
    if (!parse_release_version(tag_v->as_string(), ver)) continue;
    if (!allow_preview && !ver.prerelease.empty()) continue;
    if (compare_release_version(ver, installed) <= 0) continue;
    if (have_best && compare_release_version(ver, best_ver) <= 0) continue;

    const auto* assets = rel.get("assets");
    if (!assets || assets->kind != JsonVal::kArray) continue;
    for (const auto& asset : assets->arr) {
      if (asset.kind != JsonVal::kObject) continue;
      const auto* name_v = asset.get("name");
      if (!name_v || name_v->as_string() != platform_asset_name) continue;
      const auto* url_v = asset.get("browser_download_url");
      const auto* dig_v = asset.get("digest");
      if (!url_v || !dig_v) continue;
      const std::string digest = lower_ascii(dig_v->as_string());
      if (digest.rfind("sha256:", 0) != 0 || digest.size() != 71) continue;
      const std::string hex = digest.substr(7);
      if (!std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c); })) continue;

      AvailableUpdate u;
      u.version = ver.to_string();
      u.tag = tag_v->as_string();
      if (const auto* hu = rel.get("html_url")) u.release_url = hu->as_string();
      u.asset_name = name_v->as_string();
      u.download_url = url_v->as_string();
      if (const auto* sz = asset.get("size")) u.size_bytes = sz->as_u64();
      u.sha256 = hex;
      best = std::move(u);
      best_ver = ver;
      have_best = true;
      break;
    }
  }
  return best;
}

StagedUpdatePlan plan_staged_update(const std::filesystem::path& unpacked_dir,
                                    const std::filesystem::path& install_dir) {
  StagedUpdatePlan plan;
  plan.staging_dir = install_dir / ".shift-update";
  std::error_code ec;
  if (!std::filesystem::is_directory(unpacked_dir, ec)) return plan;
  for (const auto& e : std::filesystem::directory_iterator(unpacked_dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    plan.moves.emplace_back(e.path(), install_dir / e.path().filename());
  }
  std::sort(plan.moves.begin(), plan.moves.end(),
            [](const auto& a, const auto& b) { return a.first.filename() < b.first.filename(); });
  return plan;
}

bool verify_update_sha256(const std::filesystem::path& file, std::string_view expected_hex,
                          std::string* actual_hex) {
  std::string hex;
  if (!sha256_file(file, hex)) return false;
  if (actual_hex) *actual_hex = hex;
  return lower_ascii(hex) == lower_ascii(expected_hex);
}

}  // namespace launcher
