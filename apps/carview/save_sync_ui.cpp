#include "save_sync_ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>

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

void draw_save_page(ui::List& l, float screen_w, float screen_h, const SaveProfilePage& page,
                    const SaveSession& session) {
  if (!page.is_open()) return;
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.42f);
  ui4::title_banner(l, s, 56.0f * s, 26.0f * s, "MY PROFILE", "SAVES // BACKUPS & SYNC", true, 430.0f);

  // Top-right Profile + Sync Status badge.
  {
    const auto& sr = session.sync_report();
    const bool conflict = sr.conflict;
    const uint32_t accent = conflict ? ui4::kAmber : (sr.enabled && sr.reachable ? ui4::kGreen : ui4::kBlue);
    const float x1 = screen_w - 56.0f * s, x0 = x1 - 390.0f * s, y0 = 30.0f * s, y1 = 92.0f * s;
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, accent, 0, true);
    l.circle(x0 + 22.0f * s, y0 + 22.0f * s, 6.0f * s, accent);
    l.text(x0 + 36.0f * s, y0 + 13.0f * s, "ACTIVE GAMERTAG", ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
    ui::TextStyle gs = ui4::style(ui::Font::kE, 17.0f * s, ui4::kWhite);
    ui4::fit_text(session.gamertag(), gs, 210.0f * s, 10.0f * s);
    l.text(x0 + 36.0f * s, y0 + 32.0f * s, session.gamertag(), gs);

    const std::string sync_tag = session.isolated_mod_save() ? "MODDED SAVE"
                                 : conflict                  ? "SYNC CONFLICT"
                                 : !sr.enabled               ? "SYNC OFF"
                                 : !sr.reachable             ? "SYNC OFFLINE"
                                                             : "SYNC READY";
    l.text(x1 - 14.0f * s, y0 + 13.0f * s, sync_tag,
           ui4::style(ui::Font::kE, 11.0f * s, accent, ui::Align::kRight));
    const std::string build_tag = std::string("BUILD: ") + (session.v4_available() ? "V4" : "BASE");
    l.text(x1 - 14.0f * s, y0 + 36.0f * s, build_tag,
           ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim, ui::Align::kRight));
  }

  static const std::vector<std::string> kTabs = {"SAVE BACKUPS", "SAVE SYNC", "PROFILES", "IMPORT SAVE"};
  ui4::tab_strip(l, s, 56.0f * s, 108.0f * s, screen_w - 56.0f * s, kTabs, int(page.tab()), true, ui4::kPink);

  const float top_y = 152.0f * s, bot_y = screen_h - 82.0f * s;
  const float lx0 = 56.0f * s, lw = 700.0f * s;
  const float rx0 = lx0 + lw + 18.0f * s, rx1 = screen_w - 56.0f * s;
  const float dw = (rx1 - rx0) - 40.0f * s;

  ui4::panel(l, lx0, top_y, lx0 + lw, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, lx0 + 12.0f * s, top_y + 10.0f * s, lx0 + lw - 12.0f * s, top_y + 34.0f * s, s, false);

  ui4::panel(l, rx0, top_y, rx1, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, rx0 + 12.0f * s, top_y + 10.0f * s, rx1 - 12.0f * s, top_y + 34.0f * s, s, false);

  const float notice_reserve = page.notice().empty() ? 0.0f : 38.0f * s;
  const float row_h = 50.0f * s;
  const int max_vis = std::max(1, int((bot_y - top_y - 48.0f * s - notice_reserve) / row_h));
  float ry = top_y + 42.0f * s;

  auto kv = [&](float& y, const std::string& k, const std::string& v, uint32_t vc) {
    l.text(rx0 + 20.0f * s, y, k, ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
    ui::TextStyle vs = ui4::style(ui::Font::kE, 12.0f * s, vc);
    ui4::fit_text(v, vs, dw - 110.0f * s, 8.5f * s);
    l.text(rx0 + 128.0f * s, y - 1.0f * s, v, vs);
    y += 22.0f * s;
  };

  std::vector<ui4::Prompt> prompts;

  if (page.tab() == SaveProfilePage::Tab::kBackups) {
    const auto& list = page.backups();
    l.text(lx0 + 22.0f * s, top_y + 15.0f * s, "ROLLING SAVE BACKUPS  •  LAST 10 KEPT",
           ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
    l.text(lx0 + lw - 22.0f * s, top_y + 15.0f * s, std::to_string(list.size()) + " / 10 SNAPSHOTS",
           ui4::style(ui::Font::kB, 11.5f * s, ui4::kInk, ui::Align::kRight));
    l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "BACKUP DETAILS", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

    if (list.empty()) {
      l.text(lx0 + 24.0f * s, ry + 14.0f * s, "No backups yet. Snapshots run at start, every 10 min, and on exit.",
             ui4::style(ui::Font::kB, 12.5f * s, ui4::kDim, ui::Align::kLeft, true));
    } else {
      const int n = int(list.size());
      const int first = std::clamp(page.selected() - max_vis / 2, 0, std::max(0, n - max_vis));
      for (int i = first; i < std::min(n, first + max_vis); ++i) {
        const auto& e = list[size_t(i)];
        const bool sel = i == page.selected();
        if (sel) ui4::brush(l, lx0 + 6.0f * s, ry, lx0 + lw - 6.0f * s, ry + row_h - 4.0f * s, ui4::kPink);
        else
          l.rect(lx0 + 12.0f * s, ry, lx0 + lw - 12.0f * s, ry + row_h - 6.0f * s,
                 (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
        const std::string stamp = profile::backup::format_time(e.time);
        l.text(lx0 + 24.0f * s, ry + 8.0f * s, stamp + " UTC",
               ui4::style(sel ? ui::Font::kE : ui::Font::kB, 14.5f * s, ui4::kWhite));
        const std::string sub = (e.car.empty() ? std::string("CAR UNKNOWN") : e.car) + "  •  " +
                                ui4::format_credits(e.credits) + "  •  " + std::to_string(e.files) + " FILES";
        ui::TextStyle subs = ui4::style(ui::Font::kB, 10.0f * s, sel ? ui4::kWhite : ui4::kDim);
        ui4::fit_text(sub, subs, 430.0f * s, 8.0f * s);
        l.text(lx0 + 24.0f * s, ry + 28.0f * s, sub, subs);

        const std::string rl = profile::backup::reason_label(e.reason);
        const uint32_t rc = (e.reason == "before-restore" || e.reason == "before-sync") ? ui4::kAmber
                            : e.reason == "manual"                                      ? ui4::kBlue
                                                                                        : ui4::kGreen;
        l.text(lx0 + lw - 22.0f * s, ry + 15.0f * s, rl,
               ui4::style(ui::Font::kE, 12.0f * s, sel ? ui4::kYellow : rc, ui::Align::kRight));
        ry += row_h;
      }
      const auto& sel_e = list[size_t(page.selected())];
      ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s, ui4::kPink);
      l.text(rx0 + 20.0f * s, top_y + 54.0f * s, profile::backup::reason_label(sel_e.reason),
             ui4::style(ui::Font::kE, 16.5f * s, ui4::kWhite));
      float y = top_y + 98.0f * s;
      kv(y, "TIMESTAMP", profile::backup::format_time(sel_e.time) + " UTC", ui4::kWhite);
      kv(y, "CAR", sel_e.car.empty() ? "-" : sel_e.car, ui4::kWhite);
      kv(y, "CREDITS", ui4::format_credits(sel_e.credits), ui4::kYellow);
      kv(y, "PAYLOAD", std::to_string(sel_e.files) + " FILES (" + format_bytes(sel_e.bytes) + ")", ui4::kWhite);
      kv(y, "SNAPSHOT ID", sel_e.id, ui4::kDim);
    }
    const float sy = bot_y - 100.0f * s;
    l.rect(rx0 + 16.0f * s, sy, rx1 - 16.0f * s, sy + 84.0f * s, ui::rgba(255, 255, 255, 14));
    ui::TextStyle hs = ui4::style(ui::Font::kB, 11.0f * s, ui4::kWhite, ui::Align::kLeft, true);
    const auto notes = wrap_text(
        "Restoring first snapshots your current save as BEFORE RESTORE, so any restore can be undone. "
        "Automatic backups run at session start, every 10 minutes after saving, and on clean exit.",
        hs, dw - 12.0f * s);
    float ny = sy + 10.0f * s;
    for (size_t i = 0; i < std::min<size_t>(notes.size(), 4); ++i, ny += 16.0f * s) l.text(rx0 + 26.0f * s, ny, notes[i], hs);

    prompts = {{"A", "RESTORE BACKUP"}, {"X", "MANUAL BACKUP"}, {"LB", "PREV TAB"}, {"RB", "NEXT TAB"}, {"B", "BACK"}};
  } else if (page.tab() == SaveProfilePage::Tab::kSync) {
    const auto& sr = session.sync_report();
    const auto& list = page.remotes();
    l.text(lx0 + 22.0f * s, top_y + 15.0f * s, "CROSS-DEVICE SAVE SYNC  •  LINEAGE TRACKED",
           ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
    const std::string status_r = sr.conflict ? "CONFLICT DETECTED"
                                 : !sr.enabled ? "SYNC OFF"
                                 : !sr.reachable ? "OFFLINE"
                                                 : std::to_string(list.size()) + " DEVICES";
    l.text(lx0 + lw - 22.0f * s, top_y + 15.0f * s, status_r,
           ui4::style(ui::Font::kB, 11.5f * s, sr.conflict ? ui4::kPink : ui4::kInk, ui::Align::kRight));
    l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "DEVICE & CONFLICT RESOLUTION",
           ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

    if (!sr.enabled) {
      l.text(lx0 + 24.0f * s, ry + 14.0f * s,
             "Save Sync is off. Pass --sync-folder <Syncthing/OneDrive/Dropbox folder> or set sync-folder in settings.txt.",
             ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim, ui::Align::kLeft, true));
    } else if (!sr.reachable) {
      l.text(lx0 + 24.0f * s, ry + 14.0f * s, sr.problem,
             ui4::style(ui::Font::kE, 14.0f * s, ui4::kRed));
    } else if (list.empty()) {
      l.text(lx0 + 24.0f * s, ry + 14.0f * s, "No other devices in the sync folder yet. Your save has been published.",
             ui4::style(ui::Font::kB, 12.5f * s, ui4::kDim, ui::Align::kLeft, true));
    } else {
      const int n = int(list.size());
      const int first = std::clamp(page.selected() - max_vis / 2, 0, std::max(0, n - max_vis));
      for (int i = first; i < std::min(n, first + max_vis); ++i) {
        const auto& r = list[size_t(i)];
        const bool sel = i == page.selected();
        const bool div = r.relation == profile::sync::Relation::kDiverged;
        if (sel) ui4::brush(l, lx0 + 6.0f * s, ry, lx0 + lw - 6.0f * s, ry + row_h - 4.0f * s,
                            div ? ui4::kAmber : ui4::kPink);
        else
          l.rect(lx0 + 12.0f * s, ry, lx0 + lw - 12.0f * s, ry + row_h - 6.0f * s,
                 (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
        const uint32_t fg = (sel && div) ? ui4::kInk : ui4::kWhite;
        l.text(lx0 + 24.0f * s, ry + 8.0f * s, upper_ascii(r.m.device_name) + "  (" + upper_ascii(r.m.platform) + ")",
               ui4::style(sel ? ui::Font::kE : ui::Font::kB, 14.5f * s, fg));
        const std::string sub = r.m.written_utc + "  •  " + (r.m.car.empty() ? std::string("-") : r.m.car) + "  •  " +
                                ui4::format_credits(r.m.credits);
        ui::TextStyle subs = ui4::style(ui::Font::kB, 10.0f * s, (sel && div) ? ui4::kInk : (sel ? ui4::kWhite : ui4::kDim));
        ui4::fit_text(sub, subs, 430.0f * s, 8.0f * s);
        l.text(lx0 + 24.0f * s, ry + 28.0f * s, sub, subs);

        const uint32_t rc = (sel && div) ? ui4::kInk : (sel ? ui4::kYellow : relation_colour(r.relation));
        l.text(lx0 + lw - 22.0f * s, ry + 15.0f * s, profile::sync::relation_label(r.relation),
               ui4::style(ui::Font::kE, 12.0f * s, rc, ui::Align::kRight));
        ry += row_h;
      }
      const auto& rem = list[size_t(page.selected())];
      const bool div = rem.relation == profile::sync::Relation::kDiverged;
      ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s,
                 div ? ui4::kAmber : ui4::kPink);
      l.text(rx0 + 20.0f * s, top_y + 54.0f * s, upper_ascii(rem.m.device_name),
             ui4::style(ui::Font::kE, 16.5f * s, div ? ui4::kInk : ui4::kWhite));
      float y = top_y + 98.0f * s;
      kv(y, "RELATION", profile::sync::relation_label(rem.relation), relation_colour(rem.relation));
      kv(y, "PLATFORM", upper_ascii(rem.m.platform) + " (" + upper_ascii(rem.m.version) + ")", ui4::kWhite);
      kv(y, "WRITTEN", rem.m.written_utc, ui4::kWhite);
      kv(y, "CAR", rem.m.car.empty() ? "-" : rem.m.car, ui4::kWhite);
      kv(y, "CREDITS", ui4::format_credits(rem.m.credits), ui4::kYellow);
      kv(y, "ARCHIVE", format_bytes(rem.m.archive_bytes) + (rem.loadable ? " (VERIFIED)" : " (" + rem.problem + ")"),
         rem.loadable ? ui4::kGreen : ui4::kRed);
    }

    const float sy = bot_y - 116.0f * s;
    l.rect(rx0 + 16.0f * s, sy, rx1 - 16.0f * s, sy + 100.0f * s, ui::rgba(255, 255, 255, 14));
    ui::TextStyle hs = ui4::style(ui::Font::kB, 11.0f * s, ui4::kWhite, ui::Align::kLeft, true);
    const std::string explain =
        sr.conflict
            ? "Two devices played independently. Press [A] to load the selected device's save at next start (with a "
              "pre-sync backup), or [X] to keep this device's save so the other device auto-pulls it."
            : "Clean continuations auto-pull at boot with a BEFORE SYNC backup. Modded profiles never sync. "
              "Device: " + session.sync_context().device_name;
    const auto notes = wrap_text(explain, hs, dw - 12.0f * s);
    float ny = sy + 10.0f * s;
    for (size_t i = 0; i < std::min<size_t>(notes.size(), 5); ++i, ny += 16.0f * s) l.text(rx0 + 26.0f * s, ny, notes[i], hs);

    prompts = {{"A", "LOAD AT NEXT START"}, {"X", "KEEP THIS DEVICE'S"}, {"Y", "PUBLISH NOW"}, {"LB", "PREV TAB"},
               {"RB", "NEXT TAB"}, {"B", "BACK"}};
  } else if (page.tab() == SaveProfilePage::Tab::kProfiles) {
    const auto& list = session.profiles().profiles();
    l.text(lx0 + 22.0f * s, top_y + 15.0f * s, "SAVE PROFILES & GAMERTAGS",
           ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
    l.text(lx0 + lw - 22.0f * s, top_y + 15.0f * s, std::to_string(list.size()) + " PROFILES",
           ui4::style(ui::Font::kB, 11.5f * s, ui4::kInk, ui::Align::kRight));
    l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "PROFILE DETAILS", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

    const int n = int(list.size());
    const int first = std::clamp(page.selected() - max_vis / 2, 0, std::max(0, n - max_vis));
    for (int i = first; i < std::min(n, first + max_vis); ++i) {
      const auto& p = list[size_t(i)];
      const bool sel = i == page.selected();
      const bool act = p.name == session.active_profile_name();
      if (sel) ui4::brush(l, lx0 + 6.0f * s, ry, lx0 + lw - 6.0f * s, ry + row_h - 4.0f * s, ui4::kPink);
      else
        l.rect(lx0 + 12.0f * s, ry, lx0 + lw - 12.0f * s, ry + row_h - 6.0f * s,
               (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
      l.text(lx0 + 24.0f * s, ry + 8.0f * s, p.gamertag,
             ui4::style(sel ? ui::Font::kE : ui::Font::kB, 15.0f * s, ui4::kWhite));
      l.text(lx0 + 24.0f * s, ry + 28.0f * s, "SLOT: " + upper_ascii(p.name),
             ui4::style(ui::Font::kB, 10.0f * s, sel ? ui4::kWhite : ui4::kDim));
      if (act)
        l.text(lx0 + lw - 22.0f * s, ry + 15.0f * s, "ACTIVE",
               ui4::style(ui::Font::kE, 12.5f * s, sel ? ui4::kYellow : ui4::kGreen, ui::Align::kRight));
      ry += row_h;
    }
    if (!list.empty()) {
      const auto& p = list[size_t(page.selected())];
      ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s, ui4::kPink);
      l.text(rx0 + 20.0f * s, top_y + 54.0f * s, p.gamertag, ui4::style(ui::Font::kE, 16.5f * s, ui4::kWhite));
      float y = top_y + 98.0f * s;
      kv(y, "PROFILE ID", p.name, ui4::kWhite);
      kv(y, "GAMERTAG", p.gamertag, ui4::kYellow);
      kv(y, "STATUS", p.name == session.active_profile_name() ? "ACTIVE PROFILE" : "INACTIVE",
         p.name == session.active_profile_name() ? ui4::kGreen : ui4::kDim);
      kv(y, "SAVE PATH", p.save_dir.filename().string(), ui4::kDim);
    }
    const float sy = bot_y - 96.0f * s;
    l.rect(rx0 + 16.0f * s, sy, rx1 - 16.0f * s, sy + 80.0f * s, ui::rgba(255, 255, 255, 14));
    ui::TextStyle hs = ui4::style(ui::Font::kB, 11.0f * s, ui4::kWhite, ui::Align::kLeft, true);
    const auto notes = wrap_text(
        "Each profile keeps its own save.txt and garage.txt. Press [X] to edit the Gamertag with the on-screen "
        "keyboard (1-15 characters), or [Y] to create a new named profile.",
        hs, dw - 12.0f * s);
    float ny = sy + 10.0f * s;
    for (size_t i = 0; i < std::min<size_t>(notes.size(), 4); ++i, ny += 16.0f * s) l.text(rx0 + 26.0f * s, ny, notes[i], hs);

    prompts = {{"A", "SWITCH PROFILE"}, {"X", "EDIT GAMERTAG"}, {"Y", "NEW PROFILE"}, {"LB", "PREV TAB"},
               {"RB", "NEXT TAB"}, {"B", "BACK"}};
  } else {
    // IMPORT SAVE tab.
    const auto& list = page.import_candidates();
    l.text(lx0 + 22.0f * s, top_y + 15.0f * s, "PINYON SHIFT & XENIA SAVE IMPORT",
           ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
    l.text(lx0 + lw - 22.0f * s, top_y + 15.0f * s, "ZERO CONSOLE KEYS",
           ui4::style(ui::Font::kB, 11.5f * s, ui4::kInk, ui::Align::kRight));
    l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "IMPORT INSPECTION", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

    if (list.empty()) {
      l.text(lx0 + 24.0f * s, ry + 14.0f * s, "Drop a plaintext export folder or ZIP in <user-data>/import, or press [X].",
             ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim, ui::Align::kLeft, true));
    } else {
      const int n = int(list.size());
      const int first = std::clamp(page.selected() - max_vis / 2, 0, std::max(0, n - max_vis));
      for (int i = first; i < std::min(n, first + max_vis); ++i) {
        const auto& c = list[size_t(i)];
        const bool sel = i == page.selected();
        const bool ready = c.detection.problem.empty();
        if (sel) ui4::brush(l, lx0 + 6.0f * s, ry, lx0 + lw - 6.0f * s, ry + row_h - 4.0f * s, ui4::kPink);
        else
          l.rect(lx0 + 12.0f * s, ry, lx0 + lw - 12.0f * s, ry + row_h - 6.0f * s,
                 (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
        l.text(lx0 + 24.0f * s, ry + 8.0f * s, upper_ascii(c.label),
               ui4::style(sel ? ui::Font::kE : ui::Font::kB, 14.5f * s, ui4::kWhite));
        ui::TextStyle subs = ui4::style(ui::Font::kB, 10.0f * s, sel ? ui4::kWhite : ui4::kDim);
        ui4::fit_text(c.path.string(), subs, 430.0f * s, 8.0f * s);
        l.text(lx0 + 24.0f * s, ry + 28.0f * s, c.path.string(), subs);
        const char* st_txt = ready ? "READY"
                             : c.detection.kind == profile::import::SourceKind::kEncryptedProfile ? "NEEDS EXPORT"
                                                                                                  : "NOT FOUND";
        const uint32_t sc = ready ? ui4::kGreen
                            : c.detection.kind == profile::import::SourceKind::kEncryptedProfile ? ui4::kAmber
                                                                                                 : ui4::kDim;
        l.text(lx0 + lw - 22.0f * s, ry + 15.0f * s, st_txt,
               ui4::style(ui::Font::kE, 12.0f * s, sel ? ui4::kYellow : sc, ui::Align::kRight));
        ry += row_h;
      }
      const auto& c = list[size_t(page.selected())];
      ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s, ui4::kPink);
      l.text(rx0 + 20.0f * s, top_y + 54.0f * s, upper_ascii(c.label), ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite));
      float y = top_y + 98.0f * s;
      const char* kind_s = c.detection.kind == profile::import::SourceKind::kExportZip        ? "EXPORT ZIP"
                           : c.detection.kind == profile::import::SourceKind::kExportFolder   ? "PLAINTEXT EXPORT"
                           : c.detection.kind == profile::import::SourceKind::kEncryptedProfile ? "ENCRYPTED CONSOLE SAVE"
                                                                                                : "NONE";
      kv(y, "SOURCE KIND", kind_s, c.detection.problem.empty() ? ui4::kGreen : ui4::kAmber);
      kv(y, "TARGET BUILD", session.v4_available() ? "V4 (ACCEPTS BASE & V4)" : "BASE (BASE SAVES ONLY)", ui4::kWhite);
      kv(y, "SAFETY", "PRE-IMPORT BACKUP + .PRE-IMPORT", ui4::kGreen);
      if (!c.detection.problem.empty()) {
        y += 6.0f * s;
        ui::TextStyle ps = ui4::style(ui::Font::kB, 11.0f * s, ui4::kAmber, ui::Align::kLeft, true);
        const auto plines = wrap_text(c.detection.problem, ps, dw);
        for (size_t i = 0; i < std::min<size_t>(plines.size(), 5); ++i, y += 16.0f * s)
          l.text(rx0 + 20.0f * s, y, plines[i], ps);
      }
    }
    const float sy = bot_y - 84.0f * s;
    l.rect(rx0 + 16.0f * s, sy, rx1 - 16.0f * s, sy + 68.0f * s, ui::rgba(255, 255, 255, 14));
    ui::TextStyle hs = ui4::style(ui::Font::kB, 10.5f * s, ui4::kWhite, ui::Align::kLeft, true);
    const auto notes = wrap_text(
        "CLI one-step import: profile-import --install <folder or .zip>. Accepts Pinyon Shift plaintext exports "
        "and ZIPs; keeps .pre-import copies and checks v4 vs base compatibility.",
        hs, dw - 12.0f * s);
    float ny = sy + 10.0f * s;
    for (size_t i = 0; i < std::min<size_t>(notes.size(), 3); ++i, ny += 16.0f * s) l.text(rx0 + 26.0f * s, ny, notes[i], hs);

    prompts = {{"A", "IMPORT SAVE"}, {"X", "CUSTOM PATH"}, {"LB", "PREV TAB"}, {"RB", "NEXT TAB"}, {"B", "BACK"}};
  }

  if (!page.notice().empty()) {
    l.rect(lx0 + 12.0f * s, bot_y - 34.0f * s, lx0 + lw - 12.0f * s, bot_y - 8.0f * s, ui::rgba(14, 15, 20, 220));
    ui::TextStyle ns = ui4::style(ui::Font::kE, 12.0f * s, ui4::kYellow, ui::Align::kCenter);
    ui4::fit_text(page.notice(), ns, lw - 44.0f * s, 9.0f * s);
    l.text(lx0 + lw * 0.5f, bot_y - 28.0f * s, page.notice(), ns);
  }

  ui4::button_bar(l, prompts, screen_w, screen_h, 56.0f);

  // Slice 9 virtual keyboard on top when editing Gamertag or custom path.
  if (page.osk_open()) page.osk().draw(l, screen_w, screen_h);
}

}  // namespace carview
