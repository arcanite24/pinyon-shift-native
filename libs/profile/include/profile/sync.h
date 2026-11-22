// Cross-device save sync through a folder-sync app (ours, Slice 13; the
// design follows horizon1-recomp's docs/SAVE_SYNC.md).
//
// The player points the game at a folder that Syncthing, OneDrive,
// Dropbox, a NAS share or a USB stick keeps the same on every device
// (--sync-folder <dir> or the settings file). Each device writes only its
// own files in <sync folder>/devices/, so a sync app never sees two writers
// of one file:
//
//   <device-id>.lsave   the save folder's files (our archive format)
//   <device-id>.toml    the manifest, written last
//
// Manifest (flat TOML):
//   format = 1
//   device_id = "8c1e..."   32 hex digits, random, made once per device
//   device_name = "MULTIVAC"
//   platform = "windows"
//   version = "v4"          the build that published it: "base" or "v4"
//   written_utc = "2026-11-22T18:02:11Z"
//   written = 1795365731
//   save_hash = "..."       SHA-256 over the sorted save file names, sizes, contents
//   based_on = "..."        the newest entry of history
//   history = "h1,h2,..."   up to 64 earlier save hashes, newest first
//   archive_sha256 = "..."  archive_bytes = 2048
//   car = "..."  credits = 117700
//
// The device's own state is <user data>/config/save_sync.toml: its id, the
// machine name the id belongs to (a copied state folder gets a new id),
// `current` (the hash of the save it has), `history`, `published` and a
// pending import. Lineage decides, clocks never do; timestamps are shown.
//
// Relation of a remote R to this device: same (R's hash is current), local
// ahead (R's hash is in our history), remote ahead (we have no save, or our
// current is in R's history), diverged (neither), needs v4 (a base build
// never loads a save the v4 build published).
//
// At start (boot): the live save advances `current` when it changed since
// the last export; a chosen remote is imported, else exactly one newest
// loadable remote-ahead save is imported without asking (after a
// before-sync backup); then this device publishes when its save changed. A
// diverged remote is a conflict for the player to settle on the Save Sync
// page: load that device's save at the next start, or keep this device's
// (its hash joins our history, so the other device then loads ours).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "profile/backup.h"

namespace profile::sync {

enum class Relation { kSame, kLocalAhead, kRemoteAhead, kDiverged, kNeedsV4 };
const char* relation_label(Relation r);

struct DeviceState {
  std::string device_id, machine, current, published, pending_import;
  std::vector<std::string> history;  // newest first
  int64_t last_export = 0;
};

struct Manifest {
  int format = 1;
  std::string device_id, device_name, platform, version = "base", written_utc, save_hash, based_on;
  std::vector<std::string> history;
  std::string archive_sha256;
  uint64_t archive_bytes = 0;
  int64_t written = 0;
  std::string car;
  int64_t credits = 0;
};

struct Remote {
  Manifest m;
  std::filesystem::path archive;
  Relation relation = Relation::kDiverged;
  bool loadable = false;  // archive whole and the build can load it
  std::string problem;
};

struct Context {
  std::filesystem::path save_dir;     // the player's own save folder (never user-modded)
  std::filesystem::path state_file;   // <user data>/config/save_sync.toml
  std::filesystem::path sync_folder;  // empty: sync off
  std::string device_name, platform = "windows", build = "base";
  backup::Config backups;             // for the before-sync snapshot
  int64_t now = -1;                   // UTC seconds (-1: the clock)
};

// SHA-256 over the save folder's plain files (sorted names, sizes, bytes);
// "" when there are none.
std::string save_hash(const std::filesystem::path& save_dir);
bool pack(const std::filesystem::path& save_dir, std::vector<uint8_t>& archive);
bool unpack(const std::vector<uint8_t>& archive, const std::filesystem::path& dir, std::string* error);

bool load_state(const std::filesystem::path& file, DeviceState& out);
bool save_state(const std::filesystem::path& file, const DeviceState& s);
std::string new_device_id();

std::string manifest_to_text(const Manifest& m);
bool manifest_from_text(const std::string& text, Manifest& out);

Relation relation(const DeviceState& s, const Manifest& m, const std::string& build);
std::vector<Remote> remotes(const Context& ctx, const DeviceState& s);

struct BootReport {
  bool enabled = false, reachable = false;
  std::string problem;  // "THE SYNC FOLDER IS NOT REACHABLE"
  bool advanced = false, imported = false, published = false;
  std::string imported_from;  // device name
  std::vector<Remote> remotes;
  bool conflict = false;      // a loadable diverged remote
  std::vector<std::string> notes;
};
BootReport boot(const Context& ctx);

// Publishes this device's save now (export after a snapshot or at exit).
bool publish(const Context& ctx, std::string* error);
// Settles a remote: load its save at the next start, or keep ours.
bool choose_load(const Context& ctx, const std::string& device_id, std::string* error);
bool choose_keep(const Context& ctx, const std::string& device_id, std::string* error);

}  // namespace profile::sync
