#include "profile/sync.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

#include "sha256.h"

namespace profile::sync {
namespace fs = std::filesystem;
namespace {

constexpr size_t kHistory = 64;

int64_t utc_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string iso_utc(int64_t t) {
  const std::time_t tt = std::time_t(t);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

bool read_bytes(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

bool write_atomic(const fs::path& p, const std::string& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(data.data(), std::streamsize(data.size()))) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

std::vector<fs::path> save_files(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string n = e.path().filename().string();
    if (n.ends_with(".tmp") || n == "mods.lock" || n.ends_with(".pre-import")) continue;
    out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Flat `key = "value"` / `key = number` lines.
std::map<std::string, std::string> parse_flat(const std::string& text) {
  std::map<std::string, std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!k.empty() && k.back() == ' ') k.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ')) v.pop_back();
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
      std::string s;
      for (size_t i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == '\\' && i + 2 < v.size()) {
          ++i;
          s += v[i] == 'n' ? '\n' : v[i];
        } else {
          s += v[i];
        }
      }
      v = s;
    }
    out[k] = v;
  }
  return out;
}

std::string q(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out + "\"";
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s + ",") {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur += c;
    }
  }
  return out;
}

std::string join_csv(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + v[i];
  return out;
}

bool is_device_id(const std::string& s) {
  return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

void push_history(DeviceState& s, const std::string& h) {
  if (h.empty()) return;
  s.history.erase(std::remove(s.history.begin(), s.history.end(), h), s.history.end());
  s.history.insert(s.history.begin(), h);
  if (s.history.size() > kHistory) s.history.resize(kHistory);
}

void peek_save(const fs::path& save, std::string& car, int64_t& credits) {
  std::ifstream in(save);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("car ", 0) == 0) car = line.substr(4);
    else if (line.rfind("credits ", 0) == 0) credits = std::strtoll(line.c_str() + 8, nullptr, 10);
  }
}

// The state with an id that belongs to this machine.
DeviceState ensure_state(const Context& ctx) {
  DeviceState s;
  load_state(ctx.state_file, s);
  if (!is_device_id(s.device_id) || s.machine != ctx.device_name) {
    s.device_id = new_device_id();
    s.machine = ctx.device_name;
    s.published.clear();
  }
  return s;
}

bool import_remote(const Context& ctx, DeviceState& s, const Remote& r, std::string* error) {
  std::vector<uint8_t> archive;
  if (!read_bytes(r.archive, archive)) {
    if (error) *error = "cannot read " + r.archive.string();
    return false;
  }
  // Stage, check, back up, then put in place.
  std::error_code ec;
  const fs::path stage = ctx.save_dir.parent_path() / (".sync-stage-" + s.device_id);
  fs::remove_all(stage, ec);
  if (!unpack(archive, stage, error)) return false;
  if (save_hash(stage) != r.m.save_hash) {
    fs::remove_all(stage, ec);
    if (error) *error = "the archive from " + r.m.device_name + " does not match its manifest";
    return false;
  }
  if (!ctx.backups.backups_dir.empty()) {
    std::string why;
    if (!backup::snapshot(ctx.backups, "before-sync", nullptr, &why, ctx.now)) {
      fs::remove_all(stage, ec);
      if (error) *error = "backup before sync failed: " + why;
      return false;
    }
  }
  fs::create_directories(ctx.save_dir, ec);
  for (const auto& f : save_files(stage)) {
    const fs::path dst = ctx.save_dir / f.filename();
    fs::path tmp = dst;
    tmp += ".tmp";
    fs::copy_file(f, tmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(tmp, dst, ec);
    if (ec) {
      if (error) *error = "cannot replace " + dst.string() + ": " + ec.message();
      fs::remove_all(stage, ec);
      return false;
    }
  }
  fs::remove_all(stage, ec);
  push_history(s, s.current);
  for (auto it = r.m.history.rbegin(); it != r.m.history.rend(); ++it)
    if (*it != r.m.save_hash && std::find(s.history.begin(), s.history.end(), *it) == s.history.end())
      s.history.push_back(*it);
  if (s.history.size() > kHistory) s.history.resize(kHistory);
  s.current = r.m.save_hash;
  return true;
}

bool publish_state(const Context& ctx, DeviceState& s, std::string* error) {
  if (s.current.empty()) return true;  // nothing to publish yet
  std::vector<uint8_t> archive;
  if (!pack(ctx.save_dir, archive)) {
    if (error) *error = "cannot pack the save";
    return false;
  }
  const fs::path devices = ctx.sync_folder / "devices";
  std::error_code ec;
  fs::create_directories(devices, ec);
  Manifest m;
  m.device_id = s.device_id;
  m.device_name = ctx.device_name;
  m.platform = ctx.platform;
  m.version = ctx.build;
  const int64_t now = ctx.now >= 0 ? ctx.now : utc_now();
  m.written = now;
  m.written_utc = iso_utc(now);
  m.save_hash = s.current;
  m.history = s.history;
  m.based_on = s.history.empty() ? std::string() : s.history.front();
  m.archive_sha256 = detail::sha256_hex(archive);
  m.archive_bytes = archive.size();
  peek_save(ctx.save_dir / "save.txt", m.car, m.credits);
  if (!write_atomic(devices / (s.device_id + ".lsave"), std::string(archive.begin(), archive.end())) ||
      !write_atomic(devices / (s.device_id + ".toml"), manifest_to_text(m))) {
    if (error) *error = "cannot write to " + devices.string();
    return false;
  }
  s.published = s.current;
  s.last_export = now;
  return true;
}

}  // namespace

const char* relation_label(Relation r) {
  switch (r) {
    case Relation::kSame: return "SAME SAVE";
    case Relation::kLocalAhead: return "OLDER";
    case Relation::kRemoteAhead: return "NEWER";
    case Relation::kDiverged: return "DIFFERENT";
    case Relation::kNeedsV4: return "NEEDS THE V4 BUILD";
  }
  return "?";
}

std::string save_hash(const fs::path& save_dir) {
  const auto files = save_files(save_dir);
  if (files.empty()) return {};
  detail::Sha256 h;
  for (const auto& f : files) {
    std::vector<uint8_t> bytes;
    read_bytes(f, bytes);
    h.update(f.filename().string() + "\n" + std::to_string(bytes.size()) + "\n");
    h.update(bytes);
  }
  return h.hex();
}

bool pack(const fs::path& save_dir, std::vector<uint8_t>& archive) {
  std::string out = "shift-save-archive 1\n";
  for (const auto& f : save_files(save_dir)) {
    std::vector<uint8_t> bytes;
    if (!read_bytes(f, bytes)) return false;
    out += "file " + f.filename().string() + " " + std::to_string(bytes.size()) + "\n";
    out.append(bytes.begin(), bytes.end());
    out += "\n";
  }
  out += "end\n";
  archive.assign(out.begin(), out.end());
  return true;
}

bool unpack(const std::vector<uint8_t>& archive, const fs::path& dir, std::string* error) {
  const std::string s(archive.begin(), archive.end());
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (s.rfind("shift-save-archive 1\n", 0) != 0) return fail("not a save archive");
  size_t at = s.find('\n') + 1;
  std::error_code ec;
  fs::create_directories(dir, ec);
  while (at < s.size()) {
    const size_t nl = s.find('\n', at);
    if (nl == std::string::npos) return fail("truncated archive");
    const std::string line = s.substr(at, nl - at);
    at = nl + 1;
    if (line == "end") return true;
    if (line.rfind("file ", 0) != 0) return fail("bad archive line");
    const auto sp = line.rfind(' ');
    const std::string name = line.substr(5, sp - 5);
    const size_t size = size_t(std::strtoull(line.c_str() + sp + 1, nullptr, 10));
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..")
      return fail("bad file name in the archive");
    if (at + size + 1 > s.size()) return fail("truncated archive");
    std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
    if (!f.write(s.data() + at, std::streamsize(size))) return fail("cannot write " + name);
    at += size + 1;
  }
  return fail("archive without an end");
}

std::string new_device_id() {
  std::random_device rd;
  std::mt19937_64 gen((uint64_t(rd()) << 32) ^ rd() ^
                      uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
  static const char* d = "0123456789abcdef";
  std::string id;
  for (int i = 0; i < 32; ++i) id += d[gen() & 15];
  return id;
}

bool load_state(const fs::path& file, DeviceState& out) {
  out = DeviceState{};
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  const auto kv = parse_flat(std::string(std::istreambuf_iterator<char>(f), {}));
  auto get = [&](const char* k) {
    auto it = kv.find(k);
    return it == kv.end() ? std::string() : it->second;
  };
  out.device_id = get("device_id");
  out.machine = get("machine");
  out.current = get("current");
  out.published = get("published");
  out.pending_import = get("pending_import");
  out.history = split_csv(get("history"));
  out.last_export = std::strtoll(get("last_export").c_str(), nullptr, 10);
  return true;
}

bool save_state(const fs::path& file, const DeviceState& s) {
  std::ostringstream o;
  o << "# shift-engine save sync: this device's state (do not copy between devices)\n";
  o << "device_id = " << q(s.device_id) << "\nmachine = " << q(s.machine) << "\ncurrent = " << q(s.current)
    << "\npublished = " << q(s.published) << "\npending_import = " << q(s.pending_import)
    << "\nhistory = " << q(join_csv(s.history)) << "\nlast_export = " << s.last_export << "\n";
  return write_atomic(file, o.str());
}

std::string manifest_to_text(const Manifest& m) {
  std::ostringstream o;
  o << "format = " << m.format << "\n";
  o << "device_id = " << q(m.device_id) << "\n";
  o << "device_name = " << q(m.device_name) << "\n";
  o << "platform = " << q(m.platform) << "\n";
  o << "version = " << q(m.version) << "\n";
  o << "written_utc = " << q(m.written_utc) << "\n";
  o << "written = " << m.written << "\n";
  o << "save_hash = " << q(m.save_hash) << "\n";
  o << "based_on = " << q(m.based_on) << "\n";
  o << "history = " << q(join_csv(m.history)) << "\n";
  o << "archive_sha256 = " << q(m.archive_sha256) << "\n";
  o << "archive_bytes = " << m.archive_bytes << "\n";
  o << "car = " << q(m.car) << "\n";
  o << "credits = " << m.credits << "\n";
  return o.str();
}

bool manifest_from_text(const std::string& text, Manifest& out) {
  const auto kv = parse_flat(text);
  auto get = [&](const char* k) {
    auto it = kv.find(k);
    return it == kv.end() ? std::string() : it->second;
  };
  out = Manifest{};
  out.format = std::atoi(get("format").c_str());
  out.device_id = get("device_id");
  out.device_name = get("device_name");
  out.platform = get("platform");
  out.version = get("version").empty() ? "base" : get("version");
  out.written_utc = get("written_utc");
  out.written = std::strtoll(get("written").c_str(), nullptr, 10);
  out.save_hash = get("save_hash");
  out.based_on = get("based_on");
  out.history = split_csv(get("history"));
  out.archive_sha256 = get("archive_sha256");
  out.archive_bytes = std::strtoull(get("archive_bytes").c_str(), nullptr, 10);
  out.car = get("car");
  out.credits = std::strtoll(get("credits").c_str(), nullptr, 10);
  return out.format >= 1 && is_device_id(out.device_id) && !out.save_hash.empty();
}

Relation relation(const DeviceState& s, const Manifest& m, const std::string& build) {
  if (m.version == "v4" && build == "base") return Relation::kNeedsV4;
  if (m.save_hash == s.current) return Relation::kSame;
  if (std::find(s.history.begin(), s.history.end(), m.save_hash) != s.history.end()) return Relation::kLocalAhead;
  if (s.current.empty() || std::find(m.history.begin(), m.history.end(), s.current) != m.history.end())
    return Relation::kRemoteAhead;
  return Relation::kDiverged;
}

std::vector<Remote> remotes(const Context& ctx, const DeviceState& s) {
  std::vector<Remote> out;
  std::error_code ec;
  const fs::path devices = ctx.sync_folder / "devices";
  for (const auto& e : fs::directory_iterator(devices, ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != ".toml") continue;
    const std::string id = e.path().stem().string();
    // Sync-app conflict copies are never device ids, so they are skipped.
    if (!is_device_id(id) || id == s.device_id) continue;
    std::ifstream f(e.path(), std::ios::binary);
    Remote r;
    if (!manifest_from_text(std::string(std::istreambuf_iterator<char>(f), {}), r.m) || r.m.device_id != id) continue;
    if (r.m.format > 1) continue;  // a newer format: skipped
    r.archive = devices / (id + ".lsave");
    r.relation = relation(s, r.m, ctx.build);
    std::vector<uint8_t> bytes;
    if (!read_bytes(r.archive, bytes)) r.problem = "the save has not arrived yet";
    else if (bytes.size() != r.m.archive_bytes || detail::sha256_hex(bytes) != r.m.archive_sha256)
      r.problem = "the save is still arriving (or damaged)";
    else if (r.relation == Relation::kNeedsV4) r.problem = "needs the v4 build";
    r.loadable = r.problem.empty();
    out.push_back(std::move(r));
  }
  std::sort(out.begin(), out.end(), [](const Remote& a, const Remote& b) { return a.m.written > b.m.written; });
  return out;
}

BootReport boot(const Context& ctx) {
  BootReport rep;
  if (ctx.sync_folder.empty()) return rep;
  rep.enabled = true;
  std::error_code ec;
  if (!fs::is_directory(ctx.sync_folder, ec)) {
    rep.problem = "THE SYNC FOLDER IS NOT REACHABLE";
    return rep;  // a missing folder is reported, never created
  }
  rep.reachable = true;
  DeviceState s = ensure_state(ctx);
  // 1. Progress made since the last export advances the save.
  const std::string live = save_hash(ctx.save_dir);
  if (!live.empty() && live != s.current) {
    push_history(s, s.current);
    s.current = live;
    rep.advanced = true;
  }
  // 3-4. Import a chosen save, or exactly one newest remote-ahead save.
  auto list = remotes(ctx, s);
  const Remote* pick = nullptr;
  if (!s.pending_import.empty()) {
    for (const auto& r : list)
      if (r.m.device_id == s.pending_import && r.loadable) pick = &r;
    if (!pick) rep.notes.push_back("the chosen save is not available yet");
  } else {
    std::vector<const Remote*> ahead;
    for (const auto& r : list)
      if (r.loadable && r.relation == Relation::kRemoteAhead) ahead.push_back(&r);
    for (const Remote* a : ahead) {
      bool newest = true;  // every other ahead save is in a's lineage
      for (const Remote* b : ahead)
        if (b != a && b->m.save_hash != a->m.save_hash &&
            std::find(a->m.history.begin(), a->m.history.end(), b->m.save_hash) == a->m.history.end())
          newest = false;
      if (newest) {
        pick = a;
        break;
      }
    }
    if (!pick && ahead.size() > 1) rep.notes.push_back("several newer saves: choose one on SAVE SYNC");
  }
  if (pick) {
    std::string why;
    if (import_remote(ctx, s, *pick, &why)) {
      rep.imported = true;
      rep.imported_from = pick->m.device_name;
      s.pending_import.clear();
    } else {
      rep.notes.push_back(why);
    }
  }
  // 5. Publish when the save changed since the last export.
  if (s.current != s.published) {
    std::string why;
    if (publish_state(ctx, s, &why)) rep.published = true;
    else rep.notes.push_back(why);
  }
  save_state(ctx.state_file, s);
  rep.remotes = remotes(ctx, s);
  for (const auto& r : rep.remotes)
    if (r.loadable && r.relation == Relation::kDiverged) rep.conflict = true;
  return rep;
}

bool publish(const Context& ctx, std::string* error) {
  std::error_code ec;
  if (ctx.sync_folder.empty() || !fs::is_directory(ctx.sync_folder, ec)) {
    if (error) *error = "THE SYNC FOLDER IS NOT REACHABLE";
    return false;
  }
  DeviceState s = ensure_state(ctx);
  const std::string live = save_hash(ctx.save_dir);
  if (!live.empty() && live != s.current) push_history(s, s.current), s.current = live;
  const bool ok = s.current == s.published || publish_state(ctx, s, error);
  save_state(ctx.state_file, s);
  return ok;
}

bool choose_load(const Context& ctx, const std::string& device_id, std::string* error) {
  DeviceState s = ensure_state(ctx);
  for (const auto& r : remotes(ctx, s))
    if (r.m.device_id == device_id) {
      if (!r.loadable) {
        if (error) *error = r.problem;
        return false;
      }
      s.pending_import = device_id;
      return save_state(ctx.state_file, s);
    }
  if (error) *error = "no device " + device_id;
  return false;
}

bool choose_keep(const Context& ctx, const std::string& device_id, std::string* error) {
  DeviceState s = ensure_state(ctx);
  for (const auto& r : remotes(ctx, s))
    if (r.m.device_id == device_id) {
      // Their save joins our lineage behind ours: they see us as ahead.
      const std::string cur = s.current;
      if (std::find(s.history.begin(), s.history.end(), r.m.save_hash) == s.history.end())
        s.history.insert(s.history.begin(), r.m.save_hash);
      if (s.history.size() > kHistory) s.history.resize(kHistory);
      if (s.pending_import == device_id) s.pending_import.clear();
      s.published.clear();  // republish with the new history
      const bool ok = publish_state(ctx, s, error);
      save_state(ctx.state_file, s);
      (void)cur;
      return ok;
    }
  if (error) *error = "no device " + device_id;
  return false;
}

}  // namespace profile::sync
