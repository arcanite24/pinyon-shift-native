#include "save_sync_ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>

#include "game_hud.h"
#include "ui4_chrome.h"

namespace carview {
namespace fs = std::filesystem;
namespace {

std::string default_device_name() {
#if defined(_WIN32)
  if (const char* n = std::getenv("COMPUTERNAME"); n && *n) return n;
#else
  if (const char* n = std::getenv("HOSTNAME"); n && *n) return n;
#endif
  return "SHIFT-PC";
}

const char* host_platform() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__ANDROID__)
  return "android";
#else
  return "linux";
#endif
}

std::string upper_ascii(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string format_bytes(uint64_t b) {
  char buf[32];
  if (b >= 1024ull * 1024ull) std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / (1024.0 * 1024.0));
  else if (b >= 1024ull) std::snprintf(buf, sizeof buf, "%.1f KB", double(b) / 1024.0);
  else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
  return buf;
}

uint32_t relation_colour(profile::sync::Relation r) {
  using R = profile::sync::Relation;
  switch (r) {
    case R::kSame: return ui4::kGreen;
    case R::kLocalAhead: return ui4::kDim;
    case R::kRemoteAhead: return ui4::kBlue;
    case R::kDiverged: return ui4::kAmber;
    case R::kNeedsV4: return ui4::kRed;
  }
  return ui4::kDim;
}

}  // namespace

// ---------------------------------------------------------------- SaveSession

SaveSession::SaveSession() = default;

std::filesystem::path SaveSession::boot(const BootOptions& opts) {
  user_dir_ = opts.user_dir;
  v4_available_ = opts.v4_available;
  isolated_mod_save_ = opts.isolated_mod_save;
  log.clear();
  notices.clear();

  profiles_ = std::make_unique<profile::profiles::Manager>(user_dir_, opts.default_save_dir);
  std::string err;
  if (!profiles_->load(&err) && !err.empty()) log.push_back("profiles: " + err);
  if (!opts.profile_override.empty()) {
    if (!profiles_->switch_to(opts.profile_override, &err))
      log.push_back("profile " + opts.profile_override + ": " + err);
  }
  active_save_dir_ = profiles_->active().save_dir;
  log.push_back("profile: " + profiles_->active().name + " (" + profiles_->active().gamertag + ") -> " +
                active_save_dir_.string());

  backup_cfg_ = profile::backup::Config{};
  backup_cfg_.save_dir = active_save_dir_;
  backup_cfg_.backups_dir = user_dir_.empty() ? fs::path{} : user_dir_ / "backups" / "saves";
  backup_cfg_.achievements = opts.achievements_path;
  backup_cfg_.replays_dir = opts.replays_dir;
  backup_cfg_.keep = 10;
  auto_backup_ = std::make_unique<profile::backup::AutoBackup>(backup_cfg_);

  sync_ctx_ = profile::sync::Context{};
  sync_ctx_.save_dir = active_save_dir_;
  sync_ctx_.state_file = user_dir_.empty() ? fs::path{} : user_dir_ / "config" / "save_sync.toml";
  sync_ctx_.sync_folder = isolated_mod_save_ ? fs::path{} : opts.sync_folder;
  sync_ctx_.device_name = opts.device_name.empty() ? default_device_name() : opts.device_name;
  sync_ctx_.platform = host_platform();
  sync_ctx_.build = v4_available_ ? "v4" : "base";
  sync_ctx_.backups = backup_cfg_;
  sync_ctx_.now = opts.now;

  sync_report_ = profile::sync::BootReport{};
  if (!sync_ctx_.sync_folder.empty()) {
    sync_report_ = profile::sync::boot(sync_ctx_);
    if (!sync_report_.problem.empty()) {
      log.push_back("sync: " + sync_report_.problem);
      notices.emplace_back(sync_report_.problem, 5.0f);
    } else {
      if (sync_report_.imported) {
        const std::string msg = "LOADED THE SAVE FROM " + upper_ascii(sync_report_.imported_from);
        log.push_back("sync: " + msg);
        notices.emplace_back(msg, 5.0f);
      }
      if (sync_report_.conflict) {
        const std::string msg = "SAVE SYNC CONFLICT: CHOOSE ON PROFILE > SAVE SYNC";
        log.push_back("sync: " + msg);
        notices.emplace_back(msg, 6.0f);
      }
      if (sync_report_.published) log.push_back("sync: published save for " + sync_ctx_.device_name);
      for (const auto& n : sync_report_.notes) log.push_back("sync: " + n);
    }
  } else if (isolated_mod_save_ && !opts.sync_folder.empty()) {
    log.push_back("sync: disabled while playing an isolated modded save");
  }

  if (auto_backup_->on_session_start(0.0, opts.now)) {
    log.push_back("backup: session-start snapshot taken");
  } else if (!auto_backup_->last_error().empty()) {
    log.push_back("backup: " + auto_backup_->last_error());
  }
  return active_save_dir_;
}

void SaveSession::on_saved(double sim_time, int64_t utc) {
  if (!auto_backup_) return;
  if (auto_backup_->on_saved(sim_time, utc)) {
    if (!sync_ctx_.sync_folder.empty() && !isolated_mod_save_) {
      profile::sync::Context c = sync_ctx_;
      c.now = utc;
      std::string err;
      if (!profile::sync::publish(c, &err) && !err.empty()) log.push_back("sync: " + err);
    }
  }
}

void SaveSession::on_exit(double sim_time, int64_t utc) {
  if (auto_backup_) auto_backup_->on_exit(sim_time, utc);
  if (!sync_ctx_.sync_folder.empty() && !isolated_mod_save_) {
    profile::sync::Context c = sync_ctx_;
    c.now = utc;
    std::string err;
    if (!profile::sync::publish(c, &err) && !err.empty()) log.push_back("sync: " + err);
  }
}

std::string SaveSession::gamertag() const {
  return profiles_ ? profiles_->active().gamertag : std::string("Player");
}

std::string SaveSession::active_profile_name() const {
  return profiles_ ? profiles_->active().name : std::string("default");
}

void SaveSession::refresh_sync_remotes() {
  if (sync_ctx_.sync_folder.empty()) return;
  std::error_code ec;
  if (!fs::is_directory(sync_ctx_.sync_folder, ec)) {
    sync_report_.reachable = false;
    sync_report_.problem = "THE SYNC FOLDER IS NOT REACHABLE";
    sync_report_.remotes.clear();
    return;
  }
  profile::sync::DeviceState st;
  profile::sync::load_state(sync_ctx_.state_file, st);
  sync_report_.reachable = true;
  sync_report_.problem.clear();
  sync_report_.remotes = profile::sync::remotes(sync_ctx_, st);
  sync_report_.conflict = false;
  for (const auto& r : sync_report_.remotes)
    if (r.loadable && r.relation == profile::sync::Relation::kDiverged) sync_report_.conflict = true;
}

// ---------------------------------------------------------------- SaveProfilePage

void SaveProfilePage::open(SaveSession& session, Tab initial_tab) {
  open_ = true;
  tab_ = initial_tab;
  selected_ = 0;
  notice_.clear();
  osk_.close();
  osk_mode_ = OskMode::kNone;
  refresh(session);
}

void SaveProfilePage::set_tab(Tab t) {
  tab_ = t;
  selected_ = 0;
}

void SaveProfilePage::add_import_candidate(const fs::path& p, const std::string& label) {
  custom_imports_.emplace_back(p, label.empty() ? p.filename().string() : label);
}

void SaveProfilePage::refresh(SaveSession& session) {
  backups_ = profile::backup::list(session.backup_config());
  session.refresh_sync_remotes();
  remotes_ = session.sync_report().remotes;

  imports_.clear();
  std::vector<std::pair<fs::path, std::string>> scan_list = custom_imports_;
  if (!session.user_dir().empty()) {
    scan_list.emplace_back(session.user_dir() / "import", "USER IMPORT FOLDER");
  }
  std::error_code ec;
  if (fs::exists("out/profile", ec)) scan_list.emplace_back(fs::path("out/profile"), "LOCAL OUT/PROFILE");
  const fs::path work = fs::temp_directory_path(ec) / "shift_import_probe";
  for (const auto& [p, label] : scan_list) {
    ImportCandidate c;
    c.path = p;
    c.label = label;
    c.detection = profile::import::detect(p, work);
    imports_.push_back(std::move(c));
  }
  fs::remove_all(work, ec);
  clamp_selection(session);
}

int SaveProfilePage::current_count(const SaveSession& session) const {
  switch (tab_) {
    case Tab::kBackups: return int(backups_.size());
    case Tab::kSync: return int(remotes_.size());
    case Tab::kProfiles: return int(session.profiles().profiles().size());
    case Tab::kImport: return int(imports_.size());
    default: return 0;
  }
}

void SaveProfilePage::clamp_selection(const SaveSession& session) {
  const int n = current_count(session);
  selected_ = n > 0 ? std::clamp(selected_, 0, n - 1) : 0;
}

void SaveProfilePage::type_char(char c) {
  if (osk_.is_open()) osk_.type_char(c);
}

void SaveProfilePage::backspace() {
  if (osk_.is_open()) osk_.backspace();
}

void SaveProfilePage::commit_osk(SaveSession& session) {
  const std::string val = osk_.text();
  const OskMode mode = osk_mode_;
  osk_.close();
  osk_mode_ = OskMode::kNone;
  std::string err;
  if (mode == OskMode::kEditGamertag) {
    if (session.profiles().set_gamertag(osk_target_profile_, val, &err)) {
      notice_ = "GAMERTAG UPDATED TO " + upper_ascii(val);
    } else {
      notice_ = "GAMERTAG: " + err;
    }
  } else if (mode == OskMode::kNewProfile) {
    std::string pname = profile::profiles::name_from_gamertag(val);
    std::string candidate = pname;
    for (int k = 2; session.profiles().find(candidate) != nullptr; ++k) candidate = pname + "-" + std::to_string(k);
    if (session.profiles().create(candidate, val, &err)) {
      notice_ = "CREATED PROFILE " + upper_ascii(val) + " (" + candidate + ")";
    } else {
      notice_ = "PROFILE: " + err;
    }
  } else if (mode == OskMode::kImportPath) {
    if (!val.empty()) {
      add_import_candidate(fs::path(val), "CUSTOM IMPORT");
      notice_ = "ADDED IMPORT SOURCE: " + val;
    }
  }
  refresh(session);
}

void SaveProfilePage::nav(Nav n, SaveSession& session) {
  if (osk_.is_open()) {
    switch (n) {
      case Nav::kUp: osk_.move(0, -1); break;
      case Nav::kDown: osk_.move(0, 1); break;
      case Nav::kLeft: osk_.move(-1, 0); break;
      case Nav::kRight: osk_.move(1, 0); break;
      case Nav::kAccept:
        osk_.press_selected();
        if (osk_.confirmed()) commit_osk(session);
        else if (osk_.cancelled()) osk_.close(), osk_mode_ = OskMode::kNone;
        break;
      case Nav::kX: osk_.backspace(); break;
      case Nav::kY: osk_.toggle_shift(); break;
      case Nav::kBack:
        osk_.cancel();
        osk_mode_ = OskMode::kNone;
        break;
      default: break;
    }
    return;
  }

  const int count = current_count(session);
  switch (n) {
    case Nav::kTabPrev:
      tab_ = Tab((int(tab_) + int(Tab::kCount) - 1) % int(Tab::kCount));
      selected_ = 0;
      refresh(session);
      break;
    case Nav::kTabNext:
      tab_ = Tab((int(tab_) + 1) % int(Tab::kCount));
      selected_ = 0;
      refresh(session);
      break;
    case Nav::kUp:
      if (count > 0) selected_ = (selected_ + count - 1) % count;
      break;
    case Nav::kDown:
      if (count > 0) selected_ = (selected_ + 1) % count;
      break;
    case Nav::kBack:
      close();
      break;
    case Nav::kAccept: {
      std::string err;
      if (tab_ == Tab::kBackups && selected_ < int(backups_.size())) {
        const auto id = backups_[size_t(selected_)].id;
        if (profile::backup::restore(session.backup_config(), id, &err)) {
          restart_recommended_ = true;
          notice_ = "RESTORED " + id + " (PREVIOUS SAVE BACKED UP)";
          refresh(session);
        } else {
          notice_ = "RESTORE FAILED: " + err;
        }
      } else if (tab_ == Tab::kSync && selected_ < int(remotes_.size())) {
        const auto& rem = remotes_[size_t(selected_)];
        if (profile::sync::choose_load(session.sync_context(), rem.m.device_id, &err)) {
          restart_recommended_ = true;
          notice_ = "WILL LOAD " + upper_ascii(rem.m.device_name) + "'S SAVE AT NEXT START";
          refresh(session);
        } else {
          notice_ = "SYNC LOAD: " + err;
        }
      } else if (tab_ == Tab::kProfiles && selected_ < int(session.profiles().profiles().size())) {
        const auto& p = session.profiles().profiles()[size_t(selected_)];
        if (session.profiles().switch_to(p.name, &err)) {
          restart_recommended_ = true;
          notice_ = "ACTIVE PROFILE SET TO " + upper_ascii(p.gamertag) + " (NEXT START)";
          refresh(session);
        } else {
          notice_ = "PROFILE SWITCH: " + err;
        }
      } else if (tab_ == Tab::kImport && selected_ < int(imports_.size())) {
        const auto& cand = imports_[size_t(selected_)];
        profile::import::Options opts;
        opts.save_dir = session.active_save_dir();
        opts.replace_existing = true;
        opts.v4_available = session.v4_available();
        opts.backups = session.backup_config();
        opts.media_of = media_of_;
        const auto res = profile::import::run(cand.path, opts);
        if (res.ok) {
          restart_recommended_ = true;
          notice_ = "IMPORTED " + std::to_string(res.garage_cars) + " CARS & " + ui4::format_credits(res.credits) +
                    " (NEXT START)";
          refresh(session);
        } else {
          notice_ = "IMPORT: " + res.error;
        }
      }
      break;
    }
    case Nav::kX: {
      std::string err;
      if (tab_ == Tab::kBackups) {
        profile::backup::Entry out;
        bool skipped = false;
        if (profile::backup::snapshot(session.backup_config(), "manual", &out, &err, -1, &skipped)) {
          notice_ = skipped ? "NO SAVE FILE YET TO BACK UP" : "MANUAL BACKUP SAVED: " + out.id;
          refresh(session);
        } else {
          notice_ = "BACKUP FAILED: " + err;
        }
      } else if (tab_ == Tab::kSync && selected_ < int(remotes_.size())) {
        const auto& rem = remotes_[size_t(selected_)];
        if (profile::sync::choose_keep(session.sync_context(), rem.m.device_id, &err)) {
          notice_ = "KEPT THIS DEVICE'S SAVE (" + upper_ascii(rem.m.device_name) + " WILL AUTO-PULL)";
          refresh(session);
        } else {
          notice_ = "SYNC KEEP: " + err;
        }
      } else if (tab_ == Tab::kProfiles && selected_ < int(session.profiles().profiles().size())) {
        const auto& p = session.profiles().profiles()[size_t(selected_)];
        osk_mode_ = OskMode::kEditGamertag;
        osk_target_profile_ = p.name;
        osk_.open("EDIT GAMERTAG (" + upper_ascii(p.name) + ")", p.gamertag, 15, OnScreenKeyboard::Purpose::kGamertag);
      } else if (tab_ == Tab::kImport) {
        osk_mode_ = OskMode::kImportPath;
        osk_.open("ENTER EXPORT FOLDER OR ZIP PATH", "", 64, OnScreenKeyboard::Purpose::kLiveryName);
      }
      break;
    }
    case Nav::kY: {
      std::string err;
      if (tab_ == Tab::kSync) {
        if (profile::sync::publish(session.sync_context(), &err)) {
          notice_ = "PUBLISHED THIS DEVICE'S SAVE TO SYNC FOLDER";
          refresh(session);
        } else {
          notice_ = "PUBLISH: " + err;
        }
      } else if (tab_ == Tab::kProfiles) {
        osk_mode_ = OskMode::kNewProfile;
        osk_target_profile_.clear();
        osk_.open("NEW PROFILE GAMERTAG", "Driver", 15, OnScreenKeyboard::Purpose::kGamertag);
      }
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------- drawing

namespace {

std::string resolve_save_car_name(const std::string& raw) {
  if (raw.empty()) return "Car Unknown";
  const std::string looked_up = car_name(raw);
  if (!looked_up.empty() && looked_up != raw) return looked_up;
  const std::string pretty = ui4::pretty_car_media_name(raw);
  return pretty.empty() ? raw : pretty;
}

}  // namespace

void draw_save_page(ui::List& l, float screen_w, float screen_h, const SaveProfilePage& page,
                    const SaveSession& session) {
  if (!page.is_open()) return;

  const auto& sr = session.sync_report();
  const bool conflict = sr.conflict;
  const uint32_t sync_accent = conflict ? ui4::kAmber : (sr.enabled && sr.reachable ? ui4::kGreen : ui4::kBlue);
  const std::string sync_tag = session.isolated_mod_save() ? "MODDED SAVE"
                               : conflict                  ? "SYNC CONFLICT"
                               : !sr.enabled               ? "SYNC OFF"
                               : !sr.reachable             ? "SYNC OFFLINE"
                                                           : "SYNC READY";

  ui4::TiltedBladeSpec spec;
  spec.title = "SAVES & SYNC";
  spec.eyebrow = "MY PROFILE // BACKUPS & SYNC";
  spec.scene_hint = "H_FIXED_SETTING_LIST";
  spec.status_kicker = "GAMERTAG // " + std::string(session.v4_available() ? "V4" : "BASE");
  spec.status_value = session.gamertag();
  spec.status_tag = sync_tag;
  spec.status_color = sync_accent;
  spec.tabs = {"SAVE BACKUPS", "SAVE SYNC", "PROFILES", "IMPORT SAVE"};
  spec.active_tab = int(page.tab());
  spec.show_tab_bumpers = true;
  spec.selected_index = page.selected();
  spec.max_visible = 7;
  spec.notice = page.notice();

  if (page.tab() == SaveProfilePage::Tab::kBackups) {
    const auto& list = page.backups();
    spec.list_header = "ROLLING SAVE BACKUPS";
    spec.list_header_right = std::to_string(list.size()) + " / 10 SNAPSHOTS";
    spec.detail_header = "BACKUP DETAILS";
    spec.empty_message = "No backups yet. Snapshots run at start, every 10 min, and on exit.";

    for (size_t i = 0; i < list.size(); ++i) {
      const auto& e = list[i];
      const std::string car_disp = resolve_save_car_name(e.car);
      const std::string rl = profile::backup::reason_label(e.reason);
      const uint32_t rc = (e.reason == "before-restore" || e.reason == "before-sync") ? ui4::kAmber
                          : e.reason == "manual"                                      ? ui4::kBlue
                                                                                      : ui4::kGreen;
      ui4::BladeRow row;
      row.label = profile::backup::format_time(e.time) + " UTC";
      row.subtitle = car_disp + "  •  " + ui4::format_credits(e.credits) + "  •  " + std::to_string(e.files) + " FILES";
      row.value = rl;
      row.value_color = rc;
      row.selected = int(i) == page.selected();
      spec.rows.push_back(std::move(row));
    }

    if (!list.empty()) {
      const int idx = std::clamp(page.selected(), 0, int(list.size()) - 1);
      const auto& sel_e = list[size_t(idx)];
      const std::string car_disp = resolve_save_car_name(sel_e.car);
      spec.detail_title = profile::backup::reason_label(sel_e.reason);
      spec.detail_title_color = ui4::kPink;
      spec.detail_kvs.push_back({"TIMESTAMP", profile::backup::format_time(sel_e.time) + " UTC", ui4::kWhite});
      spec.detail_kvs.push_back({"CAR", car_disp, ui4::kWhite});
      spec.detail_kvs.push_back({"CREDITS", ui4::format_credits(sel_e.credits), ui4::kYellow});
      spec.detail_kvs.push_back(
          {"PAYLOAD", std::to_string(sel_e.files) + " FILES (" + format_bytes(sel_e.bytes) + ")", ui4::kWhite});
      spec.detail_kvs.push_back({"SNAPSHOT ID", sel_e.id, ui4::kDim});
    }
    spec.detail_footer =
        "Restoring first snapshots your current save as BEFORE RESTORE so any restore can be undone. "
        "Automatic backups run at start, every 10 min, and on clean exit.";
    spec.prompts = {{"A", "RESTORE BACKUP"}, {"X", "MANUAL BACKUP"}, {"LB", "PREV TAB"}, {"RB", "NEXT TAB"}, {"B", "BACK"}};
  } else if (page.tab() == SaveProfilePage::Tab::kSync) {
    const auto& list = page.remotes();
    spec.list_header = "CROSS-DEVICE SAVE SYNC";
    spec.list_header_right = sr.conflict     ? "CONFLICT DETECTED"
                             : !sr.enabled   ? "SYNC OFF"
                             : !sr.reachable ? "OFFLINE"
                                             : std::to_string(list.size()) + " DEVICES";
    spec.detail_header = "DEVICE & CONFLICT RESOLUTION";
    spec.empty_message =
        !sr.enabled
            ? "Save Sync is off. Pass --sync-folder <Syncthing/OneDrive/Dropbox folder> or set sync-folder in settings.txt."
            : !sr.reachable ? sr.problem
                            : "No other devices in the sync folder yet. Your save has been published.";

    if (sr.enabled && sr.reachable) {
      for (size_t i = 0; i < list.size(); ++i) {
        const auto& r = list[i];
        const bool div = r.relation == profile::sync::Relation::kDiverged;
        const std::string car_disp = r.m.car.empty() ? std::string("-") : resolve_save_car_name(r.m.car);
        ui4::BladeRow row;
        row.label = upper_ascii(r.m.device_name) + " (" + upper_ascii(r.m.platform) + ")";
        row.subtitle = r.m.written_utc + "  •  " + car_disp + "  •  " + ui4::format_credits(r.m.credits);
        row.value = profile::sync::relation_label(r.relation);
        row.value_color = relation_colour(r.relation);
        row.selected = int(i) == page.selected();
        if (div) row.row_accent = ui4::kAmber;
        spec.rows.push_back(std::move(row));
      }
      if (!list.empty()) {
        const int idx = std::clamp(page.selected(), 0, int(list.size()) - 1);
        const auto& rem = list[size_t(idx)];
        const bool div = rem.relation == profile::sync::Relation::kDiverged;
        const std::string car_disp = rem.m.car.empty() ? std::string("-") : resolve_save_car_name(rem.m.car);
        spec.detail_title = upper_ascii(rem.m.device_name);
        spec.detail_title_color = div ? ui4::kAmber : ui4::kPink;
        spec.detail_kvs.push_back({"RELATION", profile::sync::relation_label(rem.relation), relation_colour(rem.relation)});
        spec.detail_kvs.push_back({"PLATFORM", upper_ascii(rem.m.platform) + " (" + upper_ascii(rem.m.version) + ")", ui4::kWhite});
        spec.detail_kvs.push_back({"WRITTEN", rem.m.written_utc, ui4::kWhite});
        spec.detail_kvs.push_back({"CAR", car_disp, ui4::kWhite});
        spec.detail_kvs.push_back({"CREDITS", ui4::format_credits(rem.m.credits), ui4::kYellow});
        spec.detail_kvs.push_back(
            {"ARCHIVE", format_bytes(rem.m.archive_bytes) + (rem.loadable ? " (VERIFIED)" : " (" + rem.problem + ")"),
             rem.loadable ? ui4::kGreen : ui4::kRed});
      }
    }
    spec.detail_footer =
        sr.conflict
            ? "Two devices played independently. Press [A] to load the selected device's save at next start (with a "
              "pre-sync backup), or [X] to keep this device's save."
            : "Clean continuations auto-pull at boot with a BEFORE SYNC backup. Modded profiles never sync. Device: " +
                  session.sync_context().device_name;
    spec.prompts = {{"A", "LOAD AT NEXT START"}, {"X", "KEEP THIS DEVICE'S"}, {"Y", "PUBLISH NOW"},
                    {"LB", "PREV TAB"},          {"RB", "NEXT TAB"},          {"B", "BACK"}};
  } else if (page.tab() == SaveProfilePage::Tab::kProfiles) {
    const auto& list = session.profiles().profiles();
    spec.list_header = "SAVE PROFILES & GAMERTAGS";
    spec.list_header_right = std::to_string(list.size()) + " PROFILES";
    spec.detail_header = "PROFILE DETAILS";
    spec.empty_message = "No profiles found.";

    for (size_t i = 0; i < list.size(); ++i) {
      const auto& p = list[i];
      const bool act = p.name == session.active_profile_name();
      ui4::BladeRow row;
      row.label = p.gamertag;
      row.subtitle = "SLOT: " + upper_ascii(p.name);
      row.value = act ? "ACTIVE" : "";
      row.value_color = ui4::kGreen;
      row.selected = int(i) == page.selected();
      spec.rows.push_back(std::move(row));
    }
    if (!list.empty()) {
      const int idx = std::clamp(page.selected(), 0, int(list.size()) - 1);
      const auto& p = list[size_t(idx)];
      const bool act = p.name == session.active_profile_name();
      spec.detail_title = p.gamertag;
      spec.detail_title_color = ui4::kPink;
      spec.detail_kvs.push_back({"PROFILE ID", p.name, ui4::kWhite});
      spec.detail_kvs.push_back({"GAMERTAG", p.gamertag, ui4::kYellow});
      spec.detail_kvs.push_back({"STATUS", act ? "ACTIVE PROFILE" : "INACTIVE", act ? ui4::kGreen : ui4::kDim});
      spec.detail_kvs.push_back({"SAVE PATH", p.save_dir.filename().string(), ui4::kDim});
    }
    spec.detail_footer =
        "Each profile keeps its own save.txt and garage.txt. Press [X] to edit the Gamertag with the on-screen "
        "keyboard (1-15 characters), or [Y] to create a new named profile.";
    spec.prompts = {{"A", "SWITCH PROFILE"}, {"X", "EDIT GAMERTAG"}, {"Y", "NEW PROFILE"},
                    {"LB", "PREV TAB"},      {"RB", "NEXT TAB"},      {"B", "BACK"}};
  } else {
    const auto& list = page.import_candidates();
    spec.list_header = "PINYON SHIFT & XENIA SAVE IMPORT";
    spec.list_header_right = "ZERO CONSOLE KEYS";
    spec.detail_header = "IMPORT INSPECTION";
    spec.empty_message = "Drop a plaintext export folder or ZIP in <user-data>/import, or press [X].";

    for (size_t i = 0; i < list.size(); ++i) {
      const auto& c = list[i];
      const bool ready = c.detection.problem.empty();
      const char* st_txt = ready ? "READY"
                           : c.detection.kind == profile::import::SourceKind::kEncryptedProfile ? "NEEDS EXPORT"
                                                                                                : "NOT FOUND";
      const uint32_t sc = ready ? ui4::kGreen
                          : c.detection.kind == profile::import::SourceKind::kEncryptedProfile ? ui4::kAmber
                                                                                               : ui4::kDim;
      ui4::BladeRow row;
      row.label = upper_ascii(c.label);
      row.subtitle = c.path.string();
      row.value = st_txt;
      row.value_color = sc;
      row.selected = int(i) == page.selected();
      spec.rows.push_back(std::move(row));
    }
    if (!list.empty()) {
      const int idx = std::clamp(page.selected(), 0, int(list.size()) - 1);
      const auto& c = list[size_t(idx)];
      const char* kind_s = c.detection.kind == profile::import::SourceKind::kExportZip        ? "EXPORT ZIP"
                           : c.detection.kind == profile::import::SourceKind::kExportFolder   ? "PLAINTEXT EXPORT"
                           : c.detection.kind == profile::import::SourceKind::kEncryptedProfile ? "ENCRYPTED CONSOLE SAVE"
                                                                                                : "NONE";
      spec.detail_title = upper_ascii(c.label);
      spec.detail_title_color = ui4::kPink;
      spec.detail_kvs.push_back({"SOURCE KIND", kind_s, c.detection.problem.empty() ? ui4::kGreen : ui4::kAmber});
      spec.detail_kvs.push_back({"TARGET BUILD", session.v4_available() ? "V4 (ACCEPTS BASE & V4)" : "BASE (BASE SAVES ONLY)", ui4::kWhite});
      spec.detail_kvs.push_back({"SAFETY", "PRE-IMPORT BACKUP + .PRE-IMPORT", ui4::kGreen});
      if (!c.detection.problem.empty()) spec.detail_paragraphs.push_back(c.detection.problem);
    }
    spec.detail_footer =
        "CLI one-step import: profile-import --install <folder or .zip>. Accepts Pinyon Shift plaintext exports "
        "and ZIPs; keeps .pre-import copies and checks v4 vs base compatibility.";
    spec.prompts = {{"A", "IMPORT SAVE"}, {"X", "CUSTOM PATH"}, {"LB", "PREV TAB"}, {"RB", "NEXT TAB"}, {"B", "BACK"}};
  }

  ui4::tilted_blade_screen(l, screen_w, screen_h, spec);

  // Slice 9 virtual keyboard on top when editing Gamertag or custom path.
  if (page.osk_open()) page.osk().draw(l, screen_w, screen_h);
}

}  // namespace carview
