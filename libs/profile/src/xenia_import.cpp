#include "profile/xenia_import.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <functional>
#include <system_error>

#include "drive/save.h"
#include "fh1data/archive.h"
#include "garage/garage.h"
#include "profile/drive_save.h"
#include "profile/profile.h"

namespace profile::import {
namespace fs = std::filesystem;
namespace {

bool is_export(const fs::path& dir) {
  std::error_code ec;
  return fs::is_regular_file(dir / "ForzaProfile.body", ec) && fs::is_regular_file(dir / "PlayerDatabase.sqlite", ec);
}

const char* kExportStep =
    "this is the console-encrypted profile; shift-engine carries no console keys. Export it with Pinyon Shift "
    "first (python tools/fh1-profile.py export --state-root <state> --out <folder>) and import that folder";

}  // namespace

bool extract_zip(const fs::path& zip, const fs::path& dir, std::string* error) {
  auto archive = fh1::Archive::open(zip, error);
  if (!archive) return false;
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (const auto& m : archive->members()) {
    const std::string name = fh1::normalize_member_name(m.name);
    if (name.empty() || name.back() == '/') continue;
    const fs::path rel(name);
    bool bad = rel.is_absolute() || rel.has_root_name();
    for (const auto& part : rel)
      if (part == "..") bad = true;
    if (bad) {
      if (error) *error = "unsafe path in the ZIP: " + m.name;
      return false;
    }
    auto r = archive->read(m);
    if (!r.ok()) {
      if (error) *error = m.name + ": " + r.error;
      return false;
    }
    // Keep the member's own spelling (the export's files are case-sensitive on Linux).
    std::string spelled = m.name;
    std::replace(spelled.begin(), spelled.end(), '\\', '/');
    const fs::path out = dir / fs::path(spelled);
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(r.data.data()), std::streamsize(r.data.size()))) {
      if (error) *error = "cannot write " + out.string();
      return false;
    }
  }
  return true;
}

Detection detect(const fs::path& src, const fs::path& work_dir) {
  Detection d;
  std::error_code ec;
  if (fs::is_regular_file(src, ec)) {
    std::string ext = src.extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".zip") {
      const fs::path out = work_dir / "zip";
      fs::remove_all(out, ec);
      std::string why;
      if (!extract_zip(src, out, &why)) {
        d.problem = "cannot read the ZIP: " + why;
        return d;
      }
      Detection inner = detect(out, {});
      if (inner.kind == SourceKind::kExportFolder) inner.kind = SourceKind::kExportZip;
      inner.description = "ZIP " + src.filename().string() + ": " + inner.description;
      return inner;
    }
    if (src.filename() == "ForzaProfile.body") return detect(src.parent_path(), work_dir);
    if (src.filename() == "ForzaProfile") {
      if (is_export(src.parent_path())) return detect(src.parent_path(), work_dir);
      d.kind = SourceKind::kEncryptedProfile;
      d.description = "encrypted profile " + src.string();
      d.problem = kExportStep;
      return d;
    }
    d.problem = src.filename().string() + " is not a save export, ZIP or profile";
    return d;
  }
  if (!fs::is_directory(src, ec)) {
    d.problem = src.string() + " does not exist";
    return d;
  }
  // Breadth first, 6 levels: exports (preferring a folder named user) and
  // encrypted profiles.
  std::vector<fs::path> exports;
  fs::path encrypted;
  std::deque<std::pair<fs::path, int>> queue{{src, 0}};
  while (!queue.empty()) {
    auto [dir, depth] = queue.front();
    queue.pop_front();
    if (is_export(dir)) exports.push_back(dir);
    if (encrypted.empty() && fs::is_regular_file(dir / "ForzaProfile", ec)) encrypted = dir / "ForzaProfile";
    if (depth >= 6) continue;
    std::vector<fs::path> subs;
    for (const auto& e : fs::directory_iterator(dir, ec))
      if (e.is_directory(ec)) subs.push_back(e.path());
    std::sort(subs.begin(), subs.end());
    for (const auto& s : subs) {
      const std::string n = s.filename().string();
      if (n == "0000000000000000" || n == "00000002" || n == "Headers") continue;  // DLC, not saves
      queue.emplace_back(s, depth + 1);
    }
  }
  if (!exports.empty()) {
    auto pick = std::find_if(exports.begin(), exports.end(), [](const fs::path& p) { return p.filename() == "user"; });
    d.kind = SourceKind::kExportFolder;
    d.export_dir = pick != exports.end() ? *pick : exports.front();
    d.description = "plaintext export " + d.export_dir.string();
    if (exports.size() > 1) d.description += " (" + std::to_string(exports.size()) + " exports found)";
    return d;
  }
  if (!encrypted.empty()) {
    d.kind = SourceKind::kEncryptedProfile;
    d.description = "encrypted profile " + encrypted.string();
    d.problem = kExportStep;
    return d;
  }
  d.problem = "no save export (ForzaProfile.body + PlayerDatabase.sqlite) under " + src.string();
  return d;
}

Result run(const fs::path& src, const Options& opts) {
  Result res;
  std::error_code ec;
  const fs::path work = fs::temp_directory_path(ec) / ("shift-import-" + std::to_string(std::hash<std::string>{}(src.string())));
  res.detection = detect(src, work);
  auto done = [&](bool ok) {
    fs::remove_all(work, ec);
    res.ok = ok;
    return res;
  };
  if (!res.detection.problem.empty()) {
    res.error = res.detection.problem;
    return done(false);
  }
  Imported prof;
  if (!import_profile(res.detection.export_dir, prof, &res.error)) return done(false);
  res.v4_save = prof.version_flags == 2;
  if (res.v4_save && !opts.v4_available) {
    res.error = "this save was written by the v4 title update; install v4 (setup's title update step) to play it";
    return done(false);
  }
  const fs::path save_path = opts.save_dir / "save.txt", garage_path = opts.save_dir / "garage.txt";
  drive::DriveSave existing;
  std::string why;
  const bool have_existing = drive::read_save(save_path, existing, &why);
  if (have_existing && existing.owned_garage && !opts.replace_existing) {
    res.error = "the save already holds an imported career; import again with replace to overwrite it";
    return done(false);
  }
  if (!opts.backups.backups_dir.empty()) {
    backup::Config b = opts.backups;
    b.save_dir = opts.save_dir;
    if (!backup::snapshot(b, "before-import", nullptr, &why)) res.notes.push_back("backup: " + why);
  }
  fs::create_directories(opts.save_dir, ec);
  for (const auto& p : {save_path, garage_path}) {
    fs::path keep = p;
    keep += ".pre-import";
    if (fs::exists(p, ec) && !fs::exists(keep, ec)) fs::copy_file(p, keep, ec);
  }
  drive::DriveSave save;
  if (have_existing) save = existing;  // keeps the controls and where the car stands
  std::function<std::string(int64_t)> media_of = opts.media_of;
  if (!media_of) media_of = [](int64_t) { return std::string(); };
  to_drive_save(prof, media_of, save, &res.notes);
  if (!have_existing) {
    save.position = {kFestivalSpawn[0], kFestivalSpawn[1], kFestivalSpawn[2]};
    save.heading = kFestivalSpawn[3];
  }
  if (save.car.empty()) save.car = have_existing ? existing.car : "NIS_SkylineGTRVSpecII_02";
  if (!drive::write_save(save_path, save, &res.error)) return done(false);
  const garage::Garage g = to_garage(prof);
  if (!garage::write_garage(garage_path, g, &res.error)) return done(false);
  res.credits = save.credits;
  res.garage_cars = save.garage.size();
  res.signs = save.signs_found.size();
  res.car = save.car;
  res.notes.push_back(std::string(res.v4_save ? "v4" : "base") + " save from " + res.detection.description);
  return done(true);
}

}  // namespace profile::import
