// Profile > Save Backups, Save Sync, Profiles & Import (ours, Slice 13).
//
// SaveSession coordinates the save ecosystem for carview:
//  - multi-profile & gamertag manager (profile/profiles.h);
//  - rolling automatic save backups (session start, every 10 minutes after
//    saves, and on clean exit, keeping the last 10 in <user-data>/backups/saves/);
//  - cross-device Save Sync (SAVE_SYNC.md): auto-pulls clean continuations at
//    boot (with a before-sync backup), publishes on backups and clean exit,
//    and flags diverged conflicts for the in-game Save Sync UI (never syncing
//    isolated user-modded or XE profiles);
//  - in-game save import (profile/xenia_import.h) with zero console keys.
//
// SaveProfilePage is the authentic FH1 UI4 overlay (using ui4_chrome.* and
// Slice 9's OnScreenKeyboard for gamertag editing & custom import paths).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mods_page.h"
#include "profile/backup.h"
#include "profile/profiles.h"
#include "profile/sync.h"
#include "profile/xenia_import.h"
#include "ui.h"
#include "ui_widgets.h"

namespace carview {

class SaveSession {
 public:
  SaveSession();

  struct BootOptions {
    std::filesystem::path user_dir;          // <user-data> (profiles.txt, backups/, config/)
    std::filesystem::path default_save_dir;  // <user-data>/drive
    std::filesystem::path sync_folder;       // empty: sync off
    std::string device_name;                 // empty: hostname / fallback
    std::string profile_override;            // --profile <name>
    bool v4_available = false;
    bool isolated_mod_save = false;          // true when playing user-modded / user-<mod>
    std::filesystem::path achievements_path;
    std::filesystem::path replays_dir;
    int64_t now = -1;                        // UTC seconds (-1: system clock)
  };

  // Loads profiles, runs Save Sync boot (when not isolated), takes the
  // session-start backup, and returns the active profile's save directory.
  std::filesystem::path boot(const BootOptions& opts);
  // Call after save.txt is written: snapshots if 10 minutes elapsed and
  // publishes to the sync folder when a snapshot was taken.
  void on_saved(double sim_time, int64_t utc = -1);
  // Call on clean exit: takes the exit backup and publishes to the sync folder.
  void on_exit(double sim_time, int64_t utc = -1);

  const std::filesystem::path& user_dir() const { return user_dir_; }
  const std::filesystem::path& active_save_dir() const { return active_save_dir_; }
  std::string gamertag() const;
  std::string active_profile_name() const;
  bool v4_available() const { return v4_available_; }
  bool isolated_mod_save() const { return isolated_mod_save_; }

  profile::profiles::Manager& profiles() { return *profiles_; }
  const profile::profiles::Manager& profiles() const { return *profiles_; }
  const profile::backup::Config& backup_config() const { return backup_cfg_; }
  const profile::sync::Context& sync_context() const { return sync_ctx_; }
  const profile::sync::BootReport& sync_report() const { return sync_report_; }
  void refresh_sync_remotes();

  std::vector<std::string> log;
  std::vector<std::pair<std::string, float>> notices;

 private:
  std::filesystem::path user_dir_;
  std::filesystem::path active_save_dir_;
  bool v4_available_ = false;
  bool isolated_mod_save_ = false;
  std::unique_ptr<profile::profiles::Manager> profiles_;
  profile::backup::Config backup_cfg_;
  std::unique_ptr<profile::backup::AutoBackup> auto_backup_;
  profile::sync::Context sync_ctx_;
  profile::sync::BootReport sync_report_;
};

class SaveProfilePage {
 public:
  enum class Tab : int { kBackups = 0, kSync = 1, kProfiles = 2, kImport = 3, kCount = 4 };

  struct ImportCandidate {
    std::filesystem::path path;
    std::string label;
    profile::import::Detection detection;
  };

  void open(SaveSession& session, Tab initial_tab = Tab::kBackups);
  void close() {
    open_ = false;
    osk_.close();
  }
  bool is_open() const { return open_; }
  Tab tab() const { return tab_; }
  void set_tab(Tab t);

  // Set optional car-id resolver for in-game imports (baked_ids).
  void set_media_resolver(std::function<std::string(int64_t)> fn) { media_of_ = std::move(fn); }
  // Add an extra candidate folder/ZIP for the Import tab (e.g. in tests).
  void add_import_candidate(const std::filesystem::path& p, const std::string& label = {});

  // Handles navigation input; also drives OnScreenKeyboard while active.
  void nav(Nav n, SaveSession& session);
  // Physical keyboard character / backspace while OnScreenKeyboard is open.
  void type_char(char c);
  void backspace();
  void update(double dt) {
    if (osk_.is_open()) osk_.update(dt);
  }

  bool osk_open() const { return osk_.is_open(); }
  const OnScreenKeyboard& osk() const { return osk_; }

  int selected() const { return selected_; }
  const std::vector<profile::backup::Entry>& backups() const { return backups_; }
  const std::vector<profile::sync::Remote>& remotes() const { return remotes_; }
  const std::vector<ImportCandidate>& import_candidates() const { return imports_; }
  const std::string& notice() const { return notice_; }
  void set_notice(std::string n) { notice_ = std::move(n); }
  bool restart_recommended() const { return restart_recommended_; }

  void refresh(SaveSession& session);

 private:
  enum class OskMode { kNone, kEditGamertag, kNewProfile, kImportPath };

  void commit_osk(SaveSession& session);
  int current_count(const SaveSession& session) const;
  void clamp_selection(const SaveSession& session);

  bool open_ = false;
  Tab tab_ = Tab::kBackups;
  int selected_ = 0;
  bool restart_recommended_ = false;
  std::string notice_;
  std::vector<profile::backup::Entry> backups_;
  std::vector<profile::sync::Remote> remotes_;
  std::vector<ImportCandidate> imports_;
  std::vector<std::pair<std::filesystem::path, std::string>> custom_imports_;
  std::function<std::string(int64_t)> media_of_;
  OnScreenKeyboard osk_;
  OskMode osk_mode_ = OskMode::kNone;
  std::string osk_target_profile_;
};

void draw_save_page(ui::List& l, float screen_w, float screen_h, const SaveProfilePage& page,
                    const SaveSession& session);

}  // namespace carview
