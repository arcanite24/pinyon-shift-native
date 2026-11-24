// Rolling save backups (ours, Slice 13).
//
// A snapshot copies the save folder's files (save.txt, garage.txt and any
// other plain file beside them: not the user-modded / user-<profile>
// folders), the achievements file and a list of the replays (names, sizes
// and times: replays are large and append-only, so only their metadata is
// kept) into <backups>/<id>/, with backup.txt describing it:
//
//   shift-save-backup 1
//   reason start | autosave | exit | before-restore | before-import | before-sync | manual
//   time 1795365152            (UTC seconds)
//   car NIS_SkylineGTRVSpecII_02
//   credits 117700
//   file save.txt 2048 <sha-256>
//
// The id is <UTC yyyymmdd-hhmmss>-<reason>, so names sort by time. Only the
// newest `keep` (10) snapshots stay; older ones are deleted after each new
// one. Restoring first snapshots the current save as before-restore, so a
// restore can itself be undone, then copies the snapshot's files back
// through temporary files renamed over the originals.
//
// AutoBackup drives the schedule: a snapshot at session start, after a
// save when the last snapshot is at least 10 minutes old, and at a clean
// exit.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "drive/save.h"

namespace profile::backup {

struct Config {
  std::filesystem::path save_dir;      // save.txt, garage.txt
  std::filesystem::path backups_dir;   // <user data>/backups/saves
  std::filesystem::path achievements;  // achievements.txt (optional)
  std::filesystem::path replays_dir;   // replays/ (optional: metadata only)
  size_t keep = 10;
};

struct Entry {
  std::string id, reason;
  std::filesystem::path dir;
  int64_t time = 0;  // UTC seconds
  std::string car;
  int64_t credits = 0;
  size_t files = 0;
  uint64_t bytes = 0;
};

// Takes a snapshot; false with `error` when nothing could be copied (no save
// yet is not an error: the snapshot is skipped and `skipped` set).
bool snapshot(const Config& cfg, std::string_view reason, Entry* out, std::string* error, int64_t now = -1,
              bool* skipped = nullptr);
// The snapshots, newest first.
std::vector<Entry> list(const Config& cfg);
// Deletes all but the newest `cfg.keep`; returns how many were deleted.
size_t rotate(const Config& cfg);
// Restores snapshot `id` over the save folder (after a before-restore
// snapshot of the current files).
bool restore(const Config& cfg, const std::string& id, std::string* error, int64_t now = -1);

// Loads `save_path` into `out_save`. If the primary file is missing, empty, or
// corrupt/unparseable, scans rolling snapshots under `backup_dir` newest-first
// (verifying `backup.txt` SHA-256 digests when present so tampered snapshots
// are rejected) and any `<save_path>.bak` sibling. Preserves any corrupt
// primary file as `<save_path>.corrupt` and atomically restores the newest
// valid backup over `save_path`.
bool load_save_with_recovery(const std::filesystem::path& save_path, const std::filesystem::path& backup_dir,
                             drive::DriveSave& out_save, std::filesystem::path* recovered_from = nullptr,
                             std::string* error = nullptr);

// "2026-11-22 19:32" (UTC) for the Save Backups page.
std::string format_time(int64_t utc_seconds);
std::string reason_label(std::string_view reason);

class AutoBackup {
 public:
  static constexpr double kInterval = 600.0;  // s between autosave snapshots
  explicit AutoBackup(Config cfg) : cfg_(std::move(cfg)) {}
  const Config& config() const { return cfg_; }
  // `now`: the session clock in s (monotonic); `utc`: wall time for the id
  // (-1: the system clock).
  bool on_session_start(double now, int64_t utc = -1);
  // After each successful save write: snapshots when the last one is at
  // least kInterval old.
  bool on_saved(double now, int64_t utc = -1);
  bool on_exit(double now, int64_t utc = -1);
  size_t snapshots_taken() const { return taken_; }
  const std::string& last_error() const { return error_; }

 private:
  bool take(std::string_view reason, double now, int64_t utc);
  Config cfg_;
  double last_ = -1e30;
  size_t taken_ = 0;
  std::string error_;
};

}  // namespace profile::backup
