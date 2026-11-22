#include "profile/backup.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "sha256.h"

namespace profile::backup {
namespace fs = std::filesystem;
namespace {

int64_t utc_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::tm utc_tm(int64_t t) {
  const std::time_t tt = std::time_t(t);
  std::tm out{};
#if defined(_WIN32)
  gmtime_s(&out, &tt);
#else
  gmtime_r(&tt, &out);
#endif
  return out;
}

bool read_bytes(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

bool write_bytes(const fs::path& p, const std::vector<uint8_t>& data) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  return f && f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

// car and credits from a save.txt.
void peek_save(const fs::path& save, std::string& car, int64_t& credits) {
  std::ifstream in(save);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("car ", 0) == 0) car = line.substr(4);
    else if (line.rfind("credits ", 0) == 0) credits = std::strtoll(line.c_str() + 8, nullptr, 10);
  }
}

Entry read_entry(const fs::path& dir) {
  Entry e;
  e.id = dir.filename().string();
  e.dir = dir;
  std::ifstream in(dir / "backup.txt");
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string k;
    ls >> k;
    if (k == "reason") ls >> e.reason;
    else if (k == "time") ls >> e.time;
    else if (k == "car") ls >> e.car;
    else if (k == "credits") ls >> e.credits;
    else if (k == "file") {
      std::string name;
      uint64_t size = 0;
      ls >> name >> size;
      ++e.files;
      e.bytes += size;
    }
  }
  return e;
}

// The save folder's plain files (not sub-folders, temporaries or locks).
std::vector<fs::path> save_files(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string n = e.path().filename().string();
    if (n.ends_with(".tmp") || n == "mods.lock") continue;
    out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::string format_time(int64_t t) {
  const std::tm tm = utc_tm(t);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min);
  return buf;
}

std::string reason_label(std::string_view r) {
  if (r == "start") return "SESSION START";
  if (r == "autosave") return "AUTOSAVE";
  if (r == "exit") return "CLEAN EXIT";
  if (r == "before-restore") return "BEFORE RESTORE";
  if (r == "before-import") return "BEFORE IMPORT";
  if (r == "before-sync") return "BEFORE SYNC";
  if (r == "manual") return "MANUAL";
  std::string s(r);
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool snapshot(const Config& cfg, std::string_view reason, Entry* out, std::string* error, int64_t now,
              bool* skipped) {
  if (skipped) *skipped = false;
  std::error_code ec;
  if (cfg.save_dir.empty() || cfg.backups_dir.empty()) {
    if (error) *error = "no save or backups folder";
    return false;
  }
  const auto files = save_files(cfg.save_dir);
  if (files.empty()) {
    if (skipped) *skipped = true;
    return true;  // nothing saved yet
  }
  if (now < 0) now = utc_now();
  const std::tm tm = utc_tm(now);
  char stamp[32];
  std::snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  std::string id = std::string(stamp) + "-" + std::string(reason);
  for (int n = 2; fs::exists(cfg.backups_dir / id, ec); ++n)
    id = std::string(stamp) + "-" + std::string(reason) + "-" + std::to_string(n);
  const fs::path tmp = cfg.backups_dir / ("." + id + ".tmp");
  fs::create_directories(tmp, ec);
  if (ec) {
    if (error) *error = "cannot make " + tmp.string() + ": " + ec.message();
    return false;
  }
  Entry e;
  e.id = id;
  e.reason = std::string(reason);
  e.time = now;
  peek_save(cfg.save_dir / "save.txt", e.car, e.credits);
  std::ostringstream manifest;
  manifest << "shift-save-backup 1\nreason " << reason << "\ntime " << now << "\n";
  if (!e.car.empty()) manifest << "car " << e.car << "\n";
  manifest << "credits " << e.credits << "\n";
  auto add = [&](const fs::path& src, const std::string& as) {
    std::vector<uint8_t> bytes;
    if (!read_bytes(src, bytes) || !write_bytes(tmp / as, bytes)) return false;
    manifest << "file " << as << " " << bytes.size() << " " << detail::sha256_hex(bytes) << "\n";
    ++e.files;
    e.bytes += bytes.size();
    return true;
  };
  for (const auto& f : files)
    if (!add(f, f.filename().string())) {
      fs::remove_all(tmp, ec);
      if (error) *error = "cannot copy " + f.string();
      return false;
    }
  if (!cfg.achievements.empty() && fs::is_regular_file(cfg.achievements, ec))
    add(cfg.achievements, "achievements.txt");
  if (!cfg.replays_dir.empty() && fs::is_directory(cfg.replays_dir, ec)) {
    std::ostringstream list;
    list << "shift-replays 1\n";
    for (const auto& r : fs::directory_iterator(cfg.replays_dir, ec))
      if (r.is_regular_file(ec))
        list << "replay " << r.path().filename().string() << " " << r.file_size(ec) << " "
             << static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    r.last_write_time(ec).time_since_epoch())
                    .count())
             << "\n";
    const std::string s = list.str();
    std::vector<uint8_t> bytes(s.begin(), s.end());
    write_bytes(tmp / "replays.txt", bytes);
    manifest << "meta replays.txt " << bytes.size() << "\n";
  }
  {
    std::ofstream m(tmp / "backup.txt", std::ios::binary | std::ios::trunc);
    m << manifest.str();
  }
  fs::rename(tmp, cfg.backups_dir / id, ec);
  if (ec) {
    fs::remove_all(tmp, ec);
    if (error) *error = "cannot place backup " + id;
    return false;
  }
  e.dir = cfg.backups_dir / id;
  rotate(cfg);
  if (out) *out = e;
  return true;
}

std::vector<Entry> list(const Config& cfg) {
  std::vector<Entry> out;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(cfg.backups_dir, ec)) {
    if (!d.is_directory(ec)) continue;
    const std::string n = d.path().filename().string();
    if (n.empty() || n[0] == '.' || !fs::is_regular_file(d.path() / "backup.txt", ec)) continue;
    out.push_back(read_entry(d.path()));
  }
  std::sort(out.begin(), out.end(),
            [](const Entry& a, const Entry& b) { return a.time != b.time ? a.time > b.time : a.id > b.id; });
  return out;
}

size_t rotate(const Config& cfg) {
  auto all = list(cfg);
  size_t n = 0;
  std::error_code ec;
  for (size_t i = cfg.keep; i < all.size(); ++i)
    if (fs::remove_all(all[i].dir, ec) > 0) ++n;
  return n;
}

bool restore(const Config& cfg, const std::string& id, std::string* error, int64_t now) {
  std::error_code ec;
  const fs::path src = cfg.backups_dir / id;
  if (id.empty() || id.find("..") != std::string::npos || !fs::is_regular_file(src / "backup.txt", ec)) {
    if (error) *error = "no backup " + id;
    return false;
  }
  // Read the snapshot first: rotation after the before-restore snapshot
  // must not delete what is being restored.
  struct Item {
    std::string name;
    std::vector<uint8_t> bytes;
  };
  std::vector<Item> items;
  {
    std::ifstream in(src / "backup.txt");
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("file ", 0) != 0) continue;
      std::istringstream ls(line.substr(5));
      std::string name, sha;
      uint64_t size = 0;
      ls >> name >> size >> sha;
      Item it{name, {}};
      if (!read_bytes(src / name, it.bytes) || it.bytes.size() != size ||
          (!sha.empty() && detail::sha256_hex(it.bytes) != sha)) {
        if (error) *error = "backup " + id + " is damaged (" + name + ")";
        return false;
      }
      items.push_back(std::move(it));
    }
  }
  Config keep_more = cfg;
  keep_more.keep = cfg.keep + 1;  // the restored snapshot survives this rotation
  if (!snapshot(keep_more, "before-restore", nullptr, error, now)) return false;
  fs::create_directories(cfg.save_dir, ec);
  for (const auto& it : items) {
    const fs::path dst =
        it.name == "achievements.txt" && !cfg.achievements.empty() ? cfg.achievements : cfg.save_dir / it.name;
    fs::path tmp = dst;
    tmp += ".tmp";
    if (!write_bytes(tmp, it.bytes)) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
      if (error) *error = "cannot replace " + dst.string() + ": " + ec.message();
      return false;
    }
  }
  rotate(cfg);
  return true;
}

bool AutoBackup::take(std::string_view reason, double now, int64_t utc) {
  bool skipped = false;
  if (!snapshot(cfg_, reason, nullptr, &error_, utc, &skipped)) return false;
  if (!skipped) last_ = now, ++taken_;
  return !skipped;
}

bool AutoBackup::on_session_start(double now, int64_t utc) { return take("start", now, utc); }

bool AutoBackup::on_saved(double now, int64_t utc) {
  if (now - last_ < kInterval) return false;
  return take("autosave", now, utc);
}

bool AutoBackup::on_exit(double now, int64_t utc) { return take("exit", now, utc); }

}  // namespace profile::backup
