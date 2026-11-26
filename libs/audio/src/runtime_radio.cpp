#include "audio/runtime_radio.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include "audio/bake.h"
#include "audio/stream.h"
#include "fh1data/fs_walk.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace audio {
namespace {

std::string lower_str(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string trim_str(std::string_view sv) {
  size_t b = 0;
  while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
  size_t e = sv.size();
  while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
  return std::string(sv.substr(b, e - b));
}

bool starts_with_icase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

bool is_http_url(std::string_view s) {
  return starts_with_icase(s, "http://") || starts_with_icase(s, "https://");
}

bool is_youtube_url(std::string_view s) {
  const std::string low = lower_str(s);
  return low.find("youtube.com/") != std::string::npos || low.find("youtu.be/") != std::string::npos ||
         low.find("music.youtube.com/") != std::string::npos;
}

uint64_t fnv1a64(std::string_view s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex_hash(std::string_view s) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(fnv1a64(s)));
  return std::string(buf);
}

std::string sanitize_id(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      out.push_back(c);
    } else if (!out.empty() && out.back() != '_') {
      out.push_back('_');
    }
  }
  if (out.empty()) out = hex_hash(raw);
  return out;
}

void split_artist_title(std::string_view raw, std::string& artist, std::string& title) {
  const size_t sep = raw.find(" - ");
  if (sep != std::string_view::npos && sep > 0 && sep + 3 < raw.size()) {
    artist = trim_str(raw.substr(0, sep));
    title = trim_str(raw.substr(sep + 3));
  } else {
    artist.clear();
    title = trim_str(raw);
  }
}

std::string quote_arg(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

// ------------------------------------------------------------------ minimal RFC-8259 JSON

struct JsonVal {
  enum Kind : uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };
  Kind kind = kNull;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JsonVal> arr;
  std::vector<std::pair<std::string, JsonVal>> obj;

  const JsonVal* find(std::string_view key) const {
    if (kind != kObject) return nullptr;
    for (const auto& kv : obj) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  std::string as_string(std::string_view fallback = {}) const {
    return kind == kString ? str : std::string(fallback);
  }
  bool as_bool(bool fallback = false) const { return kind == kBool ? b : fallback; }
  double as_number(double fallback = 0.0) const { return kind == kNumber ? num : fallback; }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view src) : s_(src) {}

  bool parse(JsonVal& out, std::string* err) {
    skip_ws();
    if (!parse_value(out, 0)) {
      if (err) *err = err_.empty() ? "invalid JSON" : err_;
      return false;
    }
    skip_ws();
    return true;
  }

 private:
  void skip_ws() {
    while (p_ < s_.size()) {
      const char c = s_[p_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++p_;
      } else {
        break;
      }
    }
  }

  bool fail(const char* msg) {
    if (err_.empty()) err_ = std::string(msg) + " at offset " + std::to_string(p_);
    return false;
  }

  bool parse_value(JsonVal& out, int depth) {
    if (depth > 64) return fail("JSON nesting too deep");
    skip_ws();
    if (p_ >= s_.size()) return fail("unexpected end of JSON");
    const char c = s_[p_];
    if (c == '{') return parse_object(out, depth + 1);
    if (c == '[') return parse_array(out, depth + 1);
    if (c == '"') {
      out.kind = JsonVal::kString;
      return parse_string(out.str);
    }
    if (c == 't' && s_.substr(p_, 4) == "true") {
      p_ += 4;
      out.kind = JsonVal::kBool;
      out.b = true;
      return true;
    }
    if (c == 'f' && s_.substr(p_, 5) == "false") {
      p_ += 5;
      out.kind = JsonVal::kBool;
      out.b = false;
      return true;
    }
    if (c == 'n' && s_.substr(p_, 4) == "null") {
      p_ += 4;
      out.kind = JsonVal::kNull;
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return parse_number(out);
    }
    return fail("unexpected token");
  }

  bool parse_hex4(uint32_t& code) {
    if (p_ + 4 > s_.size()) return fail("truncated unicode escape");
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[p_++];
      code <<= 4;
      if (c >= '0' && c <= '9') code |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') code |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') code |= uint32_t(c - 'A' + 10);
      else return fail("invalid hex digit in unicode escape");
    }
    return true;
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
      out.push_back(char(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(char(0xC0 | (cp >> 6)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(char(0xE0 | (cp >> 12)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (cp >> 18)));
      out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_string(std::string& out) {
    if (p_ >= s_.size() || s_[p_] != '"') return fail("expected '\"'");
    ++p_;
    out.clear();
    while (p_ < s_.size()) {
      const char c = s_[p_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (p_ >= s_.size()) return fail("truncated escape");
        const char e = s_[p_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!parse_hex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 6 <= s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
              p_ += 2;
              uint32_t lo = 0;
              if (!parse_hex4(lo)) return false;
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
              }
            }
            append_utf8(out, cp);
            break;
          }
          default: return fail("invalid escape sequence");
        }
      } else {
        out.push_back(c);
      }
    }
    return fail("unterminated string");
  }

  bool parse_number(JsonVal& out) {
    const size_t start = p_;
    if (s_[p_] == '-') ++p_;
    while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
    if (p_ < s_.size() && s_[p_] == '.') {
      ++p_;
      while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
    }
    if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
      ++p_;
      if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
      while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
    }
    std::string tok(s_.substr(start, p_ - start));
    char* end = nullptr;
    out.num = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str()) return fail("invalid number");
    out.kind = JsonVal::kNumber;
    return true;
  }

  bool parse_array(JsonVal& out, int depth) {
    ++p_;  // '['
    out.kind = JsonVal::kArray;
    skip_ws();
    if (p_ < s_.size() && s_[p_] == ']') {
      ++p_;
      return true;
    }
    while (p_ < s_.size()) {
      JsonVal elem;
      if (!parse_value(elem, depth)) return false;
      out.arr.push_back(std::move(elem));
      skip_ws();
      if (p_ < s_.size() && s_[p_] == ',') {
        ++p_;
        continue;
      }
      if (p_ < s_.size() && s_[p_] == ']') {
        ++p_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
    return fail("unterminated array");
  }

  bool parse_object(JsonVal& out, int depth) {
    ++p_;  // '{'
    out.kind = JsonVal::kObject;
    skip_ws();
    if (p_ < s_.size() && s_[p_] == '}') {
      ++p_;
      return true;
    }
    while (p_ < s_.size()) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (p_ >= s_.size() || s_[p_] != ':') return fail("expected ':'");
      ++p_;
      JsonVal val;
      if (!parse_value(val, depth)) return false;
      out.obj.emplace_back(std::move(key), std::move(val));
      skip_ws();
      if (p_ < s_.size() && s_[p_] == ',') {
        ++p_;
        continue;
      }
      if (p_ < s_.size() && s_[p_] == '}') {
        ++p_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
    return fail("unterminated object");
  }

  std::string_view s_;
  size_t p_ = 0;
  std::string err_;
};

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
          out += buf;
        } else {
          out.push_back(char(c));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

// ------------------------------------------------------------------ subprocess helpers

bool run_command_capture_stdout(const std::string& command, std::string& stdout_out, int timeout_ms = 30000) {
  stdout_out.clear();
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return false;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : write_pipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::string mutable_cmd = command;
  const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &si, &pi);
  CloseHandle(write_pipe);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    CloseHandle(read_pipe);
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1000, timeout_ms));
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(read_pipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
    stdout_out.append(buf, got);
    if (std::chrono::steady_clock::now() > deadline) {
      TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(pi.hProcess, 2000);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return exit_code == 0;
#else
  (void)timeout_ms;
  FILE* fp = popen(command.c_str(), "r");
  if (!fp) return false;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
    stdout_out.append(buf, n);
  }
  const int rc = pclose(fp);
  return rc == 0;
#endif
}

int run_hidden_process(const std::string& command) {
#ifdef _WIN32
  std::string mutable_cmd = command;
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                      &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return int(exit_code);
#else
  return std::system(command.c_str());
#endif
}

// ------------------------------------------------------------------ FFmpeg streaming PCM source

class FfmpegPipePcmSource final : public PcmSource {
 public:
  FfmpegPipePcmSource(std::string input_uri, int out_rate) : input_(std::move(input_uri)), rate_(std::max(8000, out_rate)) {}
  ~FfmpegPipePcmSource() override { close_proc(); }

  bool open() override {
    ffmpeg_ = find_ffmpeg();
    if (ffmpeg_.empty() || input_.empty()) return false;
    return spawn(0);
  }

  int rate() const override { return rate_; }
  int64_t length() const override { return -1; }

  bool seek(int64_t frame) override {
    if (frame <= 0 && open_) return true;
    return spawn(std::max<int64_t>(0, frame));
  }

  int64_t read(float* stereo, int64_t frames) override {
    if (!open_ || frames <= 0) return 0;
    const size_t want_bytes = size_t(frames) * 2 * sizeof(float);
    uint8_t* dst = reinterpret_cast<uint8_t*>(stereo);
    size_t got_total = 0;
    while (got_total < want_bytes) {
      size_t chunk = 0;
#ifdef _WIN32
      DWORD n = 0;
      if (!ReadFile(read_pipe_, dst + got_total, DWORD(want_bytes - got_total), &n, nullptr) || n == 0) break;
      chunk = size_t(n);
#else
      chunk = std::fread(dst + got_total, 1, want_bytes - got_total, pipe_);
      if (chunk == 0) break;
#endif
      got_total += chunk;
    }
    return int64_t(got_total / (2 * sizeof(float)));
  }

 private:
  bool spawn(int64_t start_frame) {
    close_proc();
    std::string cmd = quote_arg(ffmpeg_.string()) + " -hide_banner -nostdin -v error";
    if (start_frame > 0) {
      char ss[32];
      std::snprintf(ss, sizeof(ss), "%.3f", double(start_frame) / double(rate_));
      cmd += std::string(" -ss ") + ss;
    }
    if (is_http_url(input_)) {
      cmd += " -reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 3";
    }
    cmd += " -i " + quote_arg(input_) + " -vn -ac 2 -ar " + std::to_string(rate_) + " -f f32le pipe:1";

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe_, &write_pipe, &sa, 0)) {
      read_pipe_ = nullptr;
      return false;
    }
    SetHandleInformation(read_pipe_, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmd;
    const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(write_pipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
      CloseHandle(read_pipe_);
      read_pipe_ = nullptr;
      return false;
    }
    proc_ = pi.hProcess;
    CloseHandle(pi.hThread);
    open_ = true;
    return true;
#else
    pipe_ = popen(cmd.c_str(), "r");
    open_ = pipe_ != nullptr;
    return open_;
#endif
  }

  void close_proc() {
    if (!open_) return;
    open_ = false;
#ifdef _WIN32
    if (read_pipe_) {
      CloseHandle(read_pipe_);
      read_pipe_ = nullptr;
    }
    if (proc_) {
      if (WaitForSingleObject(proc_, 50) == WAIT_TIMEOUT) {
        TerminateProcess(proc_, 0);
        WaitForSingleObject(proc_, 200);
      }
      CloseHandle(proc_);
      proc_ = nullptr;
    }
#else
    if (pipe_) {
      pclose(pipe_);
      pipe_ = nullptr;
    }
#endif
  }

  std::string input_;
  std::filesystem::path ffmpeg_;
  int rate_ = 48000;
  bool open_ = false;
#ifdef _WIN32
  HANDLE read_pipe_ = nullptr;
  HANDLE proc_ = nullptr;
#else
  FILE* pipe_ = nullptr;
#endif
};

// Finds any cached audio file matching `<cache_dir>/yt_<id>.*`
std::filesystem::path find_cached_yt_audio(const std::filesystem::path& cache_dir, const std::string& cache_id) {
  std::error_code ec;
  if (cache_dir.empty() || cache_id.empty() || !std::filesystem::is_directory(cache_dir, ec)) return {};
  const std::string prefix = "yt_" + cache_id + ".";
  for (const auto& entry : std::filesystem::directory_iterator(cache_dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string fname = entry.path().filename().string();
    if (fname.rfind(prefix, 0) == 0 && entry.path().extension() != ".part" && entry.path().extension() != ".json") {
      return entry.path();
    }
  }
  return {};
}

class YoutubeCachedPcmSource final : public PcmSource {
 public:
  YoutubeCachedPcmSource(RuntimeTrack track, std::filesystem::path cache_dir, int out_rate)
      : track_(std::move(track)), cache_dir_(std::move(cache_dir)), rate_(std::max(8000, out_rate)) {}

  bool open() override {
    const std::string cid = track_.cache_id.empty() ? hex_hash(track_.uri) : sanitize_id(track_.cache_id);
    std::filesystem::path cached = find_cached_yt_audio(cache_dir_, cid);
    if (cached.empty()) {
      const std::filesystem::path ytdlp = find_ytdlp();
      if (ytdlp.empty()) return false;
      std::error_code ec;
      if (!cache_dir_.empty()) std::filesystem::create_directories(cache_dir_, ec);
      if (!cache_dir_.empty() && std::filesystem::is_directory(cache_dir_, ec)) {
        const std::filesystem::path out_tmpl = cache_dir_ / ("yt_" + cid + ".%(ext)s");
        const std::string cmd = quote_arg(ytdlp.string()) + " -f bestaudio --no-playlist --no-warnings -o " +
                                quote_arg(out_tmpl.string()) + " " + quote_arg(track_.uri);
        if (run_hidden_process(cmd) == 0) {
          cached = find_cached_yt_audio(cache_dir_, cid);
        }
      }
      if (cached.empty()) {
        // Fallback: resolve direct bestaudio stream URL without disk cache.
        std::string direct_url;
        const std::string cmd = quote_arg(ytdlp.string()) + " -g -f bestaudio --no-playlist --no-warnings " +
                                quote_arg(track_.uri);
        if (run_command_capture_stdout(cmd, direct_url, 20000)) {
          const size_t nl = direct_url.find_first_of("\r\n");
          const std::string first_line = trim_str(nl == std::string::npos ? direct_url : direct_url.substr(0, nl));
          if (is_http_url(first_line)) {
            inner_ = std::make_unique<FfmpegPipePcmSource>(first_line, rate_);
            return inner_->open();
          }
        }
        return false;
      }
    }
    if (is_music_file(cached)) {
      inner_ = make_file_source(cached);
    } else {
      inner_ = std::make_unique<FfmpegPipePcmSource>(cached.string(), rate_);
    }
    return inner_ && inner_->open();
  }

  int rate() const override { return inner_ ? inner_->rate() : rate_; }
  int64_t length() const override { return inner_ ? inner_->length() : -1; }
  bool seek(int64_t frame) override { return inner_ && inner_->seek(frame); }
  int64_t read(float* stereo, int64_t frames) override { return inner_ ? inner_->read(stereo, frames) : 0; }

 private:
  RuntimeTrack track_;
  std::filesystem::path cache_dir_;
  int rate_ = 48000;
  std::unique_ptr<PcmSource> inner_;
};

const std::vector<RuntimeTrack>& empty_tracks() {
  static const std::vector<RuntimeTrack> kEmpty;
  return kEmpty;
}

uint64_t pack_track_key(int station_idx, int track_idx) {
  return (uint64_t(uint32_t(station_idx)) << 32) | uint64_t(uint32_t(track_idx));
}

}  // namespace

// ------------------------------------------------------------------ public helpers

const char* radio_source_type_name(RadioSourceType type) {
  switch (type) {
    case RadioSourceType::kFolder: return "folder";
    case RadioSourceType::kFile: return "file";
    case RadioSourceType::kPlaylist: return "playlist";
    case RadioSourceType::kStream: return "stream";
    case RadioSourceType::kYoutube: return "youtube";
  }
  return "folder";
}

bool parse_radio_source_type(std::string_view text, RadioSourceType& out) {
  const std::string low = lower_str(trim_str(text));
  if (low == "folder" || low == "dir" || low == "directory") {
    out = RadioSourceType::kFolder;
    return true;
  }
  if (low == "file" || low == "audio") {
    out = RadioSourceType::kFile;
    return true;
  }
  if (low == "playlist" || low == "m3u" || low == "m3u8" || low == "pls") {
    out = RadioSourceType::kPlaylist;
    return true;
  }
  if (low == "stream" || low == "url" || low == "radio" || low == "http") {
    out = RadioSourceType::kStream;
    return true;
  }
  if (low == "youtube" || low == "yt" || low == "youtube_playlist") {
    out = RadioSourceType::kYoutube;
    return true;
  }
  return false;
}

bool is_runtime_audio_file(const std::filesystem::path& path) {
  const std::string ext = lower_str(path.extension().string());
  return ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".m4a" || ext == ".opus";
}

std::vector<RuntimeTrack> scan_audio_folder(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
    fh1::walk_tree(folder, [&](const std::filesystem::directory_entry& entry) {
      std::error_code fec;
      if (entry.is_regular_file(fec) && is_runtime_audio_file(entry.path()))
        files.push_back(fh1::usable_path(entry.path()));
      return true;
    });
  }
  std::sort(files.begin(), files.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
    return lower_str(a.lexically_relative(folder).generic_string()) <
           lower_str(b.lexically_relative(folder).generic_string());
  });
  std::vector<RuntimeTrack> out;
  out.reserve(files.size());
  for (const auto& p : files) {
    RuntimeTrack t;
    t.kind = RuntimeTrackKind::kLocalFile;
    t.uri = p.string();
    split_artist_title(p.stem().string(), t.artist, t.title);
    if (t.title.empty()) t.title = p.stem().string();
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<RuntimeTrack> parse_playlist_text(std::string_view text, const std::filesystem::path& base_dir,
                                              bool is_pls) {
  std::vector<RuntimeTrack> out;
  std::string s(text);
  // Strip UTF-8 BOM if present.
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    s.erase(0, 3);
  }
  std::istringstream iss(s);
  std::string line;

  if (is_pls || starts_with_icase(trim_str(s), "[playlist]")) {
    std::unordered_map<int, std::string> files;
    std::unordered_map<int, std::string> titles;
    int max_idx = 0;
    while (std::getline(iss, line)) {
      const std::string tr = trim_str(line);
      if (tr.empty() || tr[0] == ';' || tr[0] == '#' || tr[0] == '[') continue;
      const size_t eq = tr.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = lower_str(trim_str(tr.substr(0, eq)));
      const std::string val = trim_str(tr.substr(eq + 1));
      if (key.rfind("file", 0) == 0 && key.size() > 4) {
        const int idx = std::atoi(key.c_str() + 4);
        if (idx > 0) {
          files[idx] = val;
          max_idx = std::max(max_idx, idx);
        }
      } else if (key.rfind("title", 0) == 0 && key.size() > 5) {
        const int idx = std::atoi(key.c_str() + 5);
        if (idx > 0) titles[idx] = val;
      }
    }
    for (int i = 1; i <= max_idx; ++i) {
      auto it = files.find(i);
      if (it == files.end() || it->second.empty()) continue;
      RuntimeTrack t;
      const std::string& uri = it->second;
      if (is_youtube_url(uri)) {
        t.kind = RuntimeTrackKind::kYoutube;
        t.uri = uri;
        t.cache_id = hex_hash(uri);
      } else if (is_http_url(uri)) {
        t.kind = RuntimeTrackKind::kStream;
        t.uri = uri;
      } else {
        std::filesystem::path p(uri);
        if (p.is_relative() && !base_dir.empty()) p = (base_dir / p).lexically_normal();
        t.kind = RuntimeTrackKind::kLocalFile;
        t.uri = p.string();
      }
      auto tit = titles.find(i);
      if (tit != titles.end() && !tit->second.empty()) {
        split_artist_title(tit->second, t.artist, t.title);
      } else if (t.kind == RuntimeTrackKind::kLocalFile) {
        split_artist_title(std::filesystem::path(t.uri).stem().string(), t.artist, t.title);
      } else {
        t.title = uri;
      }
      out.push_back(std::move(t));
    }
    return out;
  }

  // M3U / M3U8
  std::string pending_artist;
  std::string pending_title;
  while (std::getline(iss, line)) {
    const std::string tr = trim_str(line);
    if (tr.empty()) continue;
    if (starts_with_icase(tr, "#EXTINF:")) {
      const size_t comma = tr.find(',');
      if (comma != std::string::npos && comma + 1 < tr.size()) {
        split_artist_title(tr.substr(comma + 1), pending_artist, pending_title);
      }
      continue;
    }
    if (tr[0] == '#') continue;

    RuntimeTrack t;
    if (is_youtube_url(tr)) {
      t.kind = RuntimeTrackKind::kYoutube;
      t.uri = tr;
      t.cache_id = hex_hash(tr);
    } else if (is_http_url(tr)) {
      t.kind = RuntimeTrackKind::kStream;
      t.uri = tr;
    } else {
      std::filesystem::path p(tr);
      if (p.is_relative() && !base_dir.empty()) p = (base_dir / p).lexically_normal();
      t.kind = RuntimeTrackKind::kLocalFile;
      t.uri = p.string();
    }
    if (!pending_title.empty()) {
      t.artist = std::move(pending_artist);
      t.title = std::move(pending_title);
    } else if (t.kind == RuntimeTrackKind::kLocalFile) {
      split_artist_title(std::filesystem::path(t.uri).stem().string(), t.artist, t.title);
    } else {
      t.title = tr;
    }
    pending_artist.clear();
    pending_title.clear();
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<RuntimeTrack> parse_playlist_file(const std::filesystem::path& playlist_path) {
  std::ifstream in(playlist_path, std::ios::binary);
  if (!in) return {};
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const bool is_pls = lower_str(playlist_path.extension().string()) == ".pls";
  return parse_playlist_text(text, playlist_path.parent_path(), is_pls);
}

bool parse_ytdlp_flat_playlist_json(std::string_view json, std::vector<RuntimeTrack>& out, std::string* error) {
  out.clear();
  JsonVal root;
  JsonParser parser(json);
  if (!parser.parse(root, error)) return false;
  if (root.kind != JsonVal::kObject) {
    if (error) *error = "yt-dlp JSON root is not an object";
    return false;
  }

  auto extract_entry = [&](const JsonVal& obj) {
    if (obj.kind != JsonVal::kObject) return;
    const std::string id = obj.find("id") ? obj.find("id")->as_string() : "";
    std::string url = obj.find("webpage_url") ? obj.find("webpage_url")->as_string() : "";
    if (url.empty() && obj.find("url")) url = obj.find("url")->as_string();
    if (url.empty() && !id.empty()) url = "https://www.youtube.com/watch?v=" + id;
    if (!url.empty() && !is_http_url(url) && !id.empty()) url = "https://www.youtube.com/watch?v=" + id;
    if (url.empty()) return;

    RuntimeTrack t;
    t.kind = RuntimeTrackKind::kYoutube;
    t.uri = url;
    t.cache_id = !id.empty() ? sanitize_id(id) : hex_hash(url);
    const std::string raw_title = obj.find("title") ? obj.find("title")->as_string() : "";
    std::string artist = obj.find("artist")     ? obj.find("artist")->as_string()
                         : obj.find("uploader") ? obj.find("uploader")->as_string()
                         : obj.find("channel")  ? obj.find("channel")->as_string()
                                                : "";
    if (!raw_title.empty()) {
      std::string parsed_artist, parsed_title;
      split_artist_title(raw_title, parsed_artist, parsed_title);
      t.title = !parsed_title.empty() ? parsed_title : raw_title;
      t.artist = !parsed_artist.empty() ? parsed_artist : artist;
    } else {
      t.title = !id.empty() ? id : url;
      t.artist = artist;
    }
    out.push_back(std::move(t));
  };

  if (const JsonVal* entries = root.find("entries"); entries && entries->kind == JsonVal::kArray) {
    for (const auto& e : entries->arr) extract_entry(e);
  } else {
    extract_entry(root);
  }
  return !out.empty();
}

std::filesystem::path find_ytdlp() {
  std::error_code ec;
  if (const char* env = std::getenv("SHIFT_YTDLP"); env && *env && std::filesystem::exists(env, ec)) return env;
#ifdef _WIN32
  const char sep = ';';
  const char* exe = "yt-dlp.exe";
#else
  const char sep = ':';
  const char* exe = "yt-dlp";
#endif
  if (const char* path = std::getenv("PATH")) {
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, sep)) {
      if (!dir.empty() && std::filesystem::exists(std::filesystem::path(dir) / exe, ec)) {
        return std::filesystem::path(dir) / exe;
      }
    }
  }
#ifdef _WIN32
  if (const char* local = std::getenv("LOCALAPPDATA")) {
    const std::filesystem::path packages = std::filesystem::path(local) / "Microsoft" / "WinGet" / "Packages";
    if (std::filesystem::is_directory(packages, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(packages, ec)) {
        if (lower_str(entry.path().filename().string()).find("yt-dlp") == std::string::npos) continue;
        std::filesystem::path found;
        fh1::walk_tree(entry.path(), [&](const std::filesystem::directory_entry& sub) {
          if (lower_str(sub.path().filename().string()) != "yt-dlp.exe") return true;
          found = fh1::usable_path(sub.path());
          return false;
        });
        if (!found.empty()) return found;
      }
    }
  }
#endif
  return {};
}

std::unique_ptr<PcmSource> make_runtime_track_source(const RuntimeTrack& track, const std::filesystem::path& cache_dir,
                                                     int out_rate) {
  switch (track.kind) {
    case RuntimeTrackKind::kLocalFile: {
      const std::filesystem::path p(track.uri);
      if (is_music_file(p)) return make_file_source(p);
      return std::make_unique<FfmpegPipePcmSource>(track.uri, out_rate);
    }
    case RuntimeTrackKind::kStream:
      return std::make_unique<FfmpegPipePcmSource>(track.uri, out_rate);
    case RuntimeTrackKind::kYoutube:
      return std::make_unique<YoutubeCachedPcmSource>(track, cache_dir, out_rate);
  }
  return nullptr;
}

// ------------------------------------------------------------------ JSON config

bool parse_runtime_radio_json(std::string_view json, RuntimeRadioConfig& out, std::string* error) {
  out = {};
  JsonVal root;
  JsonParser parser(json);
  if (!parser.parse(root, error)) return false;
  if (root.kind != JsonVal::kObject) {
    if (error) *error = "radio_stations.json root must be a JSON object";
    return false;
  }
  if (const JsonVal* v = root.find("version")) out.version = int(v->as_number(1.0));
  const JsonVal* stations = root.find("stations");
  if (!stations) return true;  // empty config is valid
  if (stations->kind != JsonVal::kArray) {
    if (error) *error = "'stations' must be an array";
    return false;
  }
  int auto_idx = 1;
  for (const auto& st_val : stations->arr) {
    if (st_val.kind != JsonVal::kObject) continue;
    RuntimeStationConfig st;
    if (const JsonVal* v = st_val.find("id")) st.id = trim_str(v->as_string());
    if (const JsonVal* v = st_val.find("name")) st.name = trim_str(v->as_string());
    if (const JsonVal* v = st_val.find("genre")) st.genre = trim_str(v->as_string());
    if (const JsonVal* v = st_val.find("accent_color")) {
      const std::string c = trim_str(v->as_string());
      if (!c.empty()) st.accent_color = c;
    }
    if (const JsonVal* v = st_val.find("logo_png")) st.logo_png = trim_str(v->as_string());
    if (const JsonVal* v = st_val.find("shuffle")) st.shuffle = v->as_bool(true);
    if (const JsonVal* v = st_val.find("enabled")) st.enabled = v->as_bool(true);
    if (const JsonVal* v = st_val.find("volume")) st.volume = std::clamp(float(v->as_number(1.0)), 0.0f, 2.0f);

    if (st.name.empty() && !st.id.empty()) st.name = st.id;
    if (st.id.empty()) st.id = sanitize_id(!st.name.empty() ? st.name : ("custom_" + std::to_string(auto_idx)));
    if (st.name.empty()) st.name = "Custom Station " + std::to_string(auto_idx);
    ++auto_idx;

    if (const JsonVal* sources = st_val.find("sources"); sources && sources->kind == JsonVal::kArray) {
      for (const auto& src_val : sources->arr) {
        if (src_val.kind != JsonVal::kObject) continue;
        RadioSourceSpec spec;
        const std::string type_str = src_val.find("type") ? src_val.find("type")->as_string() : "";
        if (!parse_radio_source_type(type_str, spec.type)) continue;
        if (const JsonVal* u = src_val.find("path")) spec.uri = trim_str(u->as_string());
        if (spec.uri.empty() && src_val.find("url")) spec.uri = trim_str(src_val.find("url")->as_string());
        if (spec.uri.empty() && src_val.find("uri")) spec.uri = trim_str(src_val.find("uri")->as_string());
        if (spec.uri.empty()) continue;
        if (const JsonVal* t = src_val.find("title")) spec.title = trim_str(t->as_string());
        if (const JsonVal* a = src_val.find("artist")) spec.artist = trim_str(a->as_string());
        st.sources.push_back(std::move(spec));
      }
    }
    out.stations.push_back(std::move(st));
  }
  return true;
}

std::string serialize_runtime_radio_json(const RuntimeRadioConfig& cfg) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"version\": " << cfg.version << ",\n";
  os << "  \"stations\": [";
  for (size_t i = 0; i < cfg.stations.size(); ++i) {
    const auto& st = cfg.stations[i];
    if (i > 0) os << ",";
    os << "\n    {\n";
    os << "      \"id\": " << json_escape(st.id) << ",\n";
    os << "      \"name\": " << json_escape(st.name) << ",\n";
    os << "      \"genre\": " << json_escape(st.genre) << ",\n";
    os << "      \"accent_color\": " << json_escape(st.accent_color) << ",\n";
    if (!st.logo_png.empty()) {
      os << "      \"logo_png\": " << json_escape(st.logo_png) << ",\n";
    }
    os << "      \"shuffle\": " << (st.shuffle ? "true" : "false") << ",\n";
    os << "      \"enabled\": " << (st.enabled ? "true" : "false") << ",\n";
    os << "      \"volume\": " << std::fixed << std::setprecision(2) << st.volume << ",\n";
    os << "      \"sources\": [";
    for (size_t j = 0; j < st.sources.size(); ++j) {
      const auto& src = st.sources[j];
      if (j > 0) os << ",";
      const bool is_url_type = (src.type == RadioSourceType::kStream || src.type == RadioSourceType::kYoutube);
      os << "\n        {\n";
      os << "          \"type\": " << json_escape(radio_source_type_name(src.type)) << ",\n";
      os << "          \"" << (is_url_type ? "url" : "path") << "\": " << json_escape(src.uri);
      if (!src.title.empty()) os << ",\n          \"title\": " << json_escape(src.title);
      if (!src.artist.empty()) os << ",\n          \"artist\": " << json_escape(src.artist);
      os << "\n        }";
    }
    if (!st.sources.empty()) os << "\n      ";
    os << "]\n    }";
  }
  if (!cfg.stations.empty()) os << "\n  ";
  os << "]\n}\n";
  return os.str();
}

bool load_runtime_radio_config(const std::filesystem::path& path, RuntimeRadioConfig& out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse_runtime_radio_json(text, out, error);
}

bool save_runtime_radio_config(const std::filesystem::path& path, const RuntimeRadioConfig& cfg, std::string* error) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
    const std::string text = serialize_runtime_radio_json(cfg);
    out.write(text.data(), std::streamsize(text.size()));
    if (!out) {
      if (error) *error = "write failed for " + tmp.string();
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    // Windows rename fails if target exists; remove then rename.
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
      if (error) *error = "rename failed for " + path.string() + ": " + ec.message();
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------------ RuntimeRadioManager

RuntimeRadioManager::RuntimeRadioManager() = default;

RuntimeRadioManager::~RuntimeRadioManager() { stop_yt_worker(); }

void RuntimeRadioManager::configure(const std::filesystem::path& user_config_path,
                                    const std::filesystem::path& play_config_path,
                                    const std::filesystem::path& cache_dir) {
  user_config_path_ = user_config_path;
  play_config_path_ = play_config_path;
  cache_dir_ = cache_dir;
  user_stamp_ = {};
  play_stamp_ = {};
}

RuntimeRadioManager::FileStamp RuntimeRadioManager::stat_file(const std::filesystem::path& p) {
  FileStamp st;
  if (p.empty()) return st;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) return st;
  st.exists = true;
  st.size = std::filesystem::file_size(p, ec);
  const auto wt = std::filesystem::last_write_time(p, ec);
  if (!ec) st.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wt.time_since_epoch()).count();
  return st;
}

bool RuntimeRadioManager::reload() {
  user_stamp_ = stat_file(user_config_path_);
  play_stamp_ = stat_file(play_config_path_);

  RuntimeRadioConfig play_cfg;
  RuntimeRadioConfig user_cfg;
  bool loaded_any = false;
  if (play_stamp_.exists) {
    std::string err;
    if (load_runtime_radio_config(play_config_path_, play_cfg, &err)) loaded_any = true;
  }
  if (user_stamp_.exists) {
    std::string err;
    if (load_runtime_radio_config(user_config_path_, user_cfg, &err)) loaded_any = true;
  }

  // Merge: play stations first, then user stations (user overrides matching id).
  std::vector<RuntimeStationConfig> merged = play_cfg.stations;
  for (const auto& us : user_cfg.stations) {
    auto it = std::find_if(merged.begin(), merged.end(),
                           [&](const RuntimeStationConfig& s) { return lower_str(s.id) == lower_str(us.id); });
    if (it != merged.end()) *it = us;
    else merged.push_back(us);
  }

  const uint64_t next_gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  std::vector<RuntimeStation> built;
  built.reserve(merged.size());
  std::vector<YoutubeJob> pending_yt;

  for (auto& sc : merged) {
    RuntimeStation rs;
    rs.config = sc;
    for (const auto& src : sc.sources) {
      switch (src.type) {
        case RadioSourceType::kFolder: {
          auto scanned = scan_audio_folder(src.uri);
          for (auto& t : scanned) rs.tracks.push_back(std::move(t));
          break;
        }
        case RadioSourceType::kFile: {
          std::filesystem::path p(src.uri);
          std::error_code ec;
          if (std::filesystem::is_regular_file(p, ec)) {
            RuntimeTrack t;
            t.kind = RuntimeTrackKind::kLocalFile;
            t.uri = p.string();
            split_artist_title(p.stem().string(), t.artist, t.title);
            if (!src.title.empty()) t.title = src.title;
            if (!src.artist.empty()) t.artist = src.artist;
            rs.tracks.push_back(std::move(t));
          }
          break;
        }
        case RadioSourceType::kPlaylist: {
          auto parsed = parse_playlist_file(src.uri);
          for (auto& t : parsed) {
            if (t.kind == RuntimeTrackKind::kYoutube) {
              rs.youtube_pending = true;
              pending_yt.push_back({sc.id, t.uri, t.title, t.artist, next_gen});
            } else {
              rs.tracks.push_back(std::move(t));
            }
          }
          break;
        }
        case RadioSourceType::kStream: {
          RuntimeTrack t;
          t.kind = RuntimeTrackKind::kStream;
          t.uri = src.uri;
          t.title = !src.title.empty() ? src.title : sc.name;
          t.artist = !src.artist.empty() ? src.artist : "LIVE STREAM";
          rs.tracks.push_back(std::move(t));
          break;
        }
        case RadioSourceType::kYoutube: {
          // Check cached playlist JSON first for instant startup, then also refresh async if absent.
          bool loaded_from_cache = false;
          if (!cache_dir_.empty()) {
            const std::filesystem::path meta_path = cache_dir_ / ("yt_playlist_" + hex_hash(src.uri) + ".json");
            std::ifstream mf(meta_path, std::ios::binary);
            if (mf) {
              std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
              std::vector<RuntimeTrack> cached_tracks;
              if (parse_ytdlp_flat_playlist_json(text, cached_tracks)) {
                for (auto& t : cached_tracks) {
                  if (!src.title.empty() && cached_tracks.size() == 1) t.title = src.title;
                  if (!src.artist.empty() && t.artist.empty()) t.artist = src.artist;
                  rs.tracks.push_back(std::move(t));
                }
                loaded_from_cache = true;
              }
            }
          }
          if (!loaded_from_cache) {
            rs.youtube_pending = true;
            pending_yt.push_back({sc.id, src.uri, src.title, src.artist, next_gen});
          }
          break;
        }
      }
    }
    built.push_back(std::move(rs));
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    play_cfg_ = std::move(play_cfg);
    user_cfg_ = std::move(user_cfg);
    stations_ = std::move(built);
  }
  for (auto& job : pending_yt) queue_youtube_job(std::move(job));
  return loaded_any;
}

bool RuntimeRadioManager::poll_updates() {
  bool changed = false;
  const FileStamp cur_user = stat_file(user_config_path_);
  const FileStamp cur_play = stat_file(play_config_path_);
  if (cur_user != user_stamp_ || cur_play != play_stamp_) {
    reload();
    changed = true;
  }

  std::vector<YoutubeResult> finished;
  {
    std::lock_guard<std::mutex> lk(yt_mu_);
    if (!yt_results_.empty()) finished.swap(yt_results_);
  }
  if (!finished.empty()) {
    const uint64_t cur_gen = generation_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& res : finished) {
      if (res.generation != cur_gen) continue;
      for (auto& st : stations_) {
        if (lower_str(st.config.id) == lower_str(res.station_id)) {
          for (auto& t : res.tracks) st.tracks.push_back(std::move(t));
          st.youtube_pending = false;
          changed = true;
          break;
        }
      }
    }
  }
  return changed;
}

std::vector<RuntimeStation> RuntimeRadioManager::enabled_stations() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<RuntimeStation> out;
  for (const auto& s : stations_) {
    if (s.config.enabled) out.push_back(s);
  }
  return out;
}

std::vector<RuntimeStation> RuntimeRadioManager::all_stations() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stations_;
}

bool RuntimeRadioManager::upsert_station(const RuntimeStationConfig& station, std::string* error) {
  if (user_config_path_.empty()) {
    if (error) *error = "user_config_path is not configured";
    return false;
  }
  RuntimeRadioConfig cfg;
  {
    std::lock_guard<std::mutex> lk(mu_);
    cfg = user_cfg_;
  }
  auto it = std::find_if(cfg.stations.begin(), cfg.stations.end(),
                         [&](const RuntimeStationConfig& s) { return lower_str(s.id) == lower_str(station.id); });
  if (it != cfg.stations.end()) *it = station;
  else cfg.stations.push_back(station);
  if (!save_runtime_radio_config(user_config_path_, cfg, error)) return false;
  reload();
  return true;
}

bool RuntimeRadioManager::remove_station(std::string_view station_id, std::string* error) {
  if (user_config_path_.empty()) {
    if (error) *error = "user_config_path is not configured";
    return false;
  }
  RuntimeRadioConfig cfg;
  {
    std::lock_guard<std::mutex> lk(mu_);
    cfg = user_cfg_;
  }
  const size_t before = cfg.stations.size();
  cfg.stations.erase(std::remove_if(cfg.stations.begin(), cfg.stations.end(),
                                    [&](const RuntimeStationConfig& s) {
                                      return lower_str(s.id) == lower_str(station_id);
                                    }),
                     cfg.stations.end());
  if (cfg.stations.size() == before) return false;
  if (!save_runtime_radio_config(user_config_path_, cfg, error)) return false;
  reload();
  return true;
}

void RuntimeRadioManager::start_yt_worker() {
  if (yt_thread_.joinable()) return;
  yt_stop_.store(false, std::memory_order_release);
  yt_thread_ = std::thread([this] { yt_worker_main(); });
}

void RuntimeRadioManager::stop_yt_worker() {
  yt_stop_.store(true, std::memory_order_release);
  if (yt_thread_.joinable()) yt_thread_.join();
}

void RuntimeRadioManager::queue_youtube_job(YoutubeJob job) {
  {
    std::lock_guard<std::mutex> lk(yt_mu_);
    yt_queue_.push_back(std::move(job));
  }
  start_yt_worker();
}

void RuntimeRadioManager::yt_worker_main() {
  while (!yt_stop_.load(std::memory_order_acquire)) {
    YoutubeJob job;
    bool have = false;
    {
      std::lock_guard<std::mutex> lk(yt_mu_);
      if (!yt_queue_.empty()) {
        job = std::move(yt_queue_.front());
        yt_queue_.erase(yt_queue_.begin());
        have = true;
      }
    }
    if (!have) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    if (job.generation != generation_.load(std::memory_order_acquire)) continue;

    YoutubeResult res;
    res.station_id = job.station_id;
    res.generation = job.generation;

    const std::filesystem::path ytdlp = find_ytdlp();
    if (!ytdlp.empty()) {
      std::string json_out;
      const std::string cmd = quote_arg(ytdlp.string()) +
                              " --flat-playlist --dump-single-json --no-warnings " + quote_arg(job.url);
      if (run_command_capture_stdout(cmd, json_out, 30000) && !json_out.empty()) {
        if (parse_ytdlp_flat_playlist_json(json_out, res.tracks)) {
          if (!cache_dir_.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cache_dir_, ec);
            const std::filesystem::path meta_path = cache_dir_ / ("yt_playlist_" + hex_hash(job.url) + ".json");
            std::ofstream mf(meta_path, std::ios::binary | std::ios::trunc);
            if (mf) mf.write(json_out.data(), std::streamsize(json_out.size()));
          }
          for (auto& t : res.tracks) {
            if (res.tracks.size() == 1 && !job.fallback_title.empty()) t.title = job.fallback_title;
            if (t.artist.empty() && !job.fallback_artist.empty()) t.artist = job.fallback_artist;
          }
        }
      }
    }
    // If yt-dlp wasn't found or failed, keep a direct single-entry placeholder so playback can retry.
    if (res.tracks.empty()) {
      RuntimeTrack fallback;
      fallback.kind = RuntimeTrackKind::kYoutube;
      fallback.uri = job.url;
      fallback.cache_id = hex_hash(job.url);
      fallback.title = !job.fallback_title.empty() ? job.fallback_title : "YouTube Track";
      fallback.artist = job.fallback_artist;
      res.tracks.push_back(std::move(fallback));
    }

    std::lock_guard<std::mutex> lk(yt_mu_);
    yt_results_.push_back(std::move(res));
  }
}

// ------------------------------------------------------------------ RuntimeRadioPlayer

RuntimeRadioPlayer::RuntimeRadioPlayer(int out_rate, uint64_t seed, bool non_blocking)
    : rate_(std::max(8000, out_rate)), non_blocking_(non_blocking), rng_(seed) {}

RuntimeRadioPlayer::~RuntimeRadioPlayer() {
  stop_streams();
  pool_.reset();
}

void RuntimeRadioPlayer::set_stations(std::vector<RuntimeStation> stations, const std::filesystem::path& cache_dir) {
  stop_streams();
  pool_.reset();
  stations_ = std::move(stations);
  cache_dir_ = cache_dir;
  orders_.resize(stations_.size());
  order_pos_.assign(stations_.size(), 0);
  for (size_t i = 0; i < stations_.size(); ++i) rebuild_order(int(i));
  if (selected_ >= int(stations_.size())) selected_ = -1;

  if (!stations_.empty()) {
    pool_ = std::make_unique<StreamPool>(4, 98304, [this](uint64_t key) -> std::unique_ptr<PcmSource> {
      const size_t si = size_t(key >> 32);
      const size_t ti = size_t(key & 0xFFFFFFFFu);
      if (si >= stations_.size() || ti >= stations_[si].tracks.size()) return nullptr;
      return make_runtime_track_source(stations_[si].tracks[ti], cache_dir_, rate_);
    });
  }
  if (active()) start_current();
}

const std::vector<RuntimeTrack>& RuntimeRadioPlayer::current_tracks() const {
  if (selected_ < 0 || size_t(selected_) >= stations_.size()) return empty_tracks();
  return stations_[size_t(selected_)].tracks;
}

void RuntimeRadioPlayer::rebuild_order(int station_idx) {
  if (station_idx < 0 || size_t(station_idx) >= stations_.size()) return;
  auto& ord = orders_[size_t(station_idx)];
  const auto& tr = stations_[size_t(station_idx)].tracks;
  ord.resize(tr.size());
  std::iota(ord.begin(), ord.end(), 0);
  if (stations_[size_t(station_idx)].config.shuffle && ord.size() > 1) {
    std::shuffle(ord.begin(), ord.end(), rng_);
  }
  order_pos_[size_t(station_idx)] = 0;
}

void RuntimeRadioPlayer::stop_streams() {
  if (pool_) {
    if (cur_slot_ >= 0) pool_->release(cur_slot_);
    if (next_slot_ >= 0) pool_->release(next_slot_);
  }
  cur_slot_ = -1;
  next_slot_ = -1;
  next_key_ = ~0ull;
  pos_ = 0.0;
  fade_ = 0.0f;
}

int RuntimeRadioPlayer::current_track_index() const {
  if (selected_ < 0 || size_t(selected_) >= orders_.size()) return -1;
  const auto& ord = orders_[size_t(selected_)];
  if (ord.empty()) return -1;
  const int p = std::clamp(order_pos_[size_t(selected_)], 0, int(ord.size()) - 1);
  return ord[size_t(p)];
}

int RuntimeRadioPlayer::next_order_pos(int pos) const {
  if (selected_ < 0 || size_t(selected_) >= orders_.size()) return 0;
  const auto& ord = orders_[size_t(selected_)];
  if (ord.empty()) return 0;
  return (pos + 1) % int(ord.size());
}

void RuntimeRadioPlayer::start_current() {
  if (!active() || !pool_) return;
  const int ti = current_track_index();
  if (ti < 0) return;
  const uint64_t key = pack_track_key(selected_, ti);
  if (cur_slot_ >= 0) {
    pool_->release(cur_slot_);
    cur_slot_ = -1;
  }
  if (next_slot_ >= 0 && next_key_ == key) {
    cur_slot_ = next_slot_;
    next_slot_ = -1;
    next_key_ = ~0ull;
  } else {
    if (next_slot_ >= 0) {
      pool_->release(next_slot_);
      next_slot_ = -1;
      next_key_ = ~0ull;
    }
    cur_slot_ = pool_->acquire(key, 0);
  }
  pos_ = 0.0;
  fade_ = 0.0f;
  ++track_seq_;
  ensure_prefetch();
}

void RuntimeRadioPlayer::ensure_prefetch() {
  if (!active() || !pool_) return;
  const auto& ord = orders_[size_t(selected_)];
  if (ord.size() <= 1) return;
  const int next_ti = ord[size_t(next_order_pos(order_pos_[size_t(selected_)]))];
  // Don't prefetch live HTTP streams (would open a second live connection prematurely).
  if (stations_[size_t(selected_)].tracks[size_t(next_ti)].kind == RuntimeTrackKind::kStream) return;
  const uint64_t want_key = pack_track_key(selected_, next_ti);
  if (next_slot_ >= 0 && next_key_ == want_key) return;
  if (next_slot_ >= 0) {
    pool_->release(next_slot_);
    next_slot_ = -1;
    next_key_ = ~0ull;
  }
  next_slot_ = pool_->acquire(want_key, 0);
  next_key_ = next_slot_ >= 0 ? want_key : ~0ull;
}

void RuntimeRadioPlayer::select_station(int index) {
  if (index < 0 || size_t(index) >= stations_.size()) {
    if (selected_ != -1) {
      selected_ = -1;
      stop_streams();
    }
    return;
  }
  if (selected_ == index && cur_slot_ >= 0) return;
  selected_ = index;
  stop_streams();
  if (active()) start_current();
}

void RuntimeRadioPlayer::next_track() {
  if (selected_ < 0 || size_t(selected_) >= orders_.size()) return;
  const auto& ord = orders_[size_t(selected_)];
  if (ord.empty()) return;
  order_pos_[size_t(selected_)] = (order_pos_[size_t(selected_)] + 1) % int(ord.size());
  if (active()) start_current();
}

void RuntimeRadioPlayer::prev_track() {
  if (selected_ < 0 || size_t(selected_) >= orders_.size()) return;
  const auto& ord = orders_[size_t(selected_)];
  if (ord.empty()) return;
  order_pos_[size_t(selected_)] = (order_pos_[size_t(selected_)] - 1 + int(ord.size())) % int(ord.size());
  if (active()) start_current();
}

std::string_view RuntimeRadioPlayer::current_title_view() const {
  const int ti = current_track_index();
  if (ti < 0) return {};
  return stations_[size_t(selected_)].tracks[size_t(ti)].title;
}

std::string_view RuntimeRadioPlayer::current_artist_view() const {
  const int ti = current_track_index();
  if (ti < 0) return {};
  return stations_[size_t(selected_)].tracks[size_t(ti)].artist;
}

std::string_view RuntimeRadioPlayer::current_station_name_view() const {
  if (selected_ < 0 || size_t(selected_) >= stations_.size()) return {};
  return stations_[size_t(selected_)].config.name;
}

RuntimeRadioPlayer::NowPlaying RuntimeRadioPlayer::now_playing() const {
  NowPlaying np;
  if (selected_ >= 0 && size_t(selected_) < stations_.size()) {
    np.active = active();
    np.station_index = selected_;
    np.station_id = stations_[size_t(selected_)].config.id;
    np.station_name = stations_[size_t(selected_)].config.name;
    np.title = std::string(current_title_view());
    np.artist = std::string(current_artist_view());
    np.track_seq = track_seq_;
  }
  return np;
}

void RuntimeRadioPlayer::render(float* out, int frames) {
  if (!active() || !pool_ || frames <= 0) return;
  if (cur_slot_ < 0) start_current();
  if (cur_slot_ < 0) return;
  const float station_vol = stations_[size_t(selected_)].config.volume;
  const float fade_step = 1.0f / std::max(1.0f, 0.015f * float(rate_));
  const size_t n_tracks = orders_[size_t(selected_)].size();
  int done_frames = 0;
  int skips = 0;
  while (done_frames < frames) {
    PcmStream* st = pool_->stream(cur_slot_);
    if (!st) break;
    if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned())) pool_->wait_ready(cur_slot_);
    const auto state = st->state();
    if (state == PcmStream::kFailed) {
      if (++skips >= int(n_tracks)) break;
      next_track();
      continue;
    }
    if (state != PcmStream::kReady || !st->positioned()) break;
    const double step = double(st->rate()) / double(rate_);
    const int rem = frames - done_frames;
    if (!non_blocking_) {
      const int64_t want_end = int64_t(pos_ + step * rem) + 2;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!st->source_done() && st->end() < want_end && std::chrono::steady_clock::now() < deadline) {
        st->hint(std::max<int64_t>(st->from(), int64_t(std::floor(pos_)) - 4));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    const int64_t rfrom = st->from(), rend = st->end();
    const bool eof = st->source_done();
    bool hit_eof = false;
    for (int i = 0; i < rem; ++i, ++done_frames, pos_ += step) {
      const int64_t a = int64_t(std::floor(pos_));
      if (a >= rend) {
        if (eof) {
          hit_eof = true;
          break;
        }
        continue;
      }
      if (a < rfrom) continue;
      const double frac = pos_ - double(a);
      float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
      st->sample(a, al, ar);
      if (a + 1 < rend) {
        st->sample(a + 1, bl, br);
        al = float(al + (bl - al) * frac);
        ar = float(ar + (br - ar) * frac);
      }
      fade_ = std::min(1.0f, fade_ + fade_step);
      const float g = volume_ * station_vol * fade_;
      out[2 * done_frames] += g * al;
      out[2 * done_frames + 1] += g * ar;
    }
    st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(pos_)) - 4));
    if (hit_eof) {
      if (n_tracks == 1) {
        pos_ = 0.0;
        fade_ = 0.0f;
        st->request(0);
      } else {
        next_track();
      }
      if (++skips > int(n_tracks)) break;
    } else {
      break;
    }
  }
}

}  // namespace audio
