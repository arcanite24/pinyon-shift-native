// F6 > MODS (ours, Slice 13): the in-game Mod Manager overlay and the
// session that boots the mods.
//
// ModsSession runs once at start-up: it scans <user data>/mods, reads
// mods.toml, resolves the load order, moves the save to user-modded (or a
// mod's own profile) when an active mod changes saves, lays the mods over
// the play folder and the disc, mounts UDLC when asked, and loads native
// plugins only when the player opted in. F5 (or RELOAD TEXTURES) rescans
// the replacement textures and re-uploads the world textures they cover.
//
// ModsPage is the overlay: the load order (the winner on top), a switch per
// mod, LB/RB to move the selected mod up or down, its details (author,
// version, description, what it changes, needs and conflicts, options) and
// a save indicator that says which save the next start plays. Changes are
// written to mods.toml and apply at the next start; texture reload applies
// at once.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cars/bake_format.h"
#include "cars/mod_manager.h"
#include "cars/mod_plugins.h"
#include "cars/physics.h"
#include "ui.h"

namespace carview {

// Menu navigation, from the keyboard or a gamepad (overlay_input.h maps SDL).
enum class Nav { kNone, kUp, kDown, kLeft, kRight, kAccept, kBack, kTabPrev, kTabNext, kX, kY };

class ModsSession {
 public:
  ModsSession();
  ~ModsSession();
  // Scans and resolves; returns the save folder to play (the main one, or
  // its user-modded / user-<profile> copy). `build`: kV4 with the title
  // update, else kBase.
  std::filesystem::path boot(const std::filesystem::path& mods_root, const std::filesystem::path& main_save_dir,
                             cars::modmgr::GameBuild build, const std::filesystem::path& state_root);
  // Lays the active mods over the play folder (before databases load).
  void activate(const std::filesystem::path& bake_dir, const std::filesystem::path& sound_dir,
                const std::filesystem::path& game_root);
  // Loads the active mods' native libraries when `opt_in` (the CLI flag) or
  // mods.toml's native_plugins allows it.
  void load_plugins(bool opt_in);
  void tick(const ShiftModFrame& frame);
  void shutdown();

  // The world-texture upload hook (the streaming worker): applies a
  // replacement and remembers the record for live reload. Thread-safe.
  void on_world_texture(uint32_t record, bool srgb, cars::bake::Texture& texture);
  // F5: rescans; calls `reupload(record, texture, srgb)` for every resident
  // world texture whose replacement changed (the original is restored when
  // its replacement was removed). Returns the changed keys.
  using Reupload = std::function<bool(uint32_t record, const cars::bake::Texture& texture, bool srgb)>;
  std::vector<std::string> reload_textures(const Reupload& reupload, size_t* reuploaded = nullptr);

  const std::filesystem::path& root() const { return root_; }
  const std::vector<cars::modmgr::ModInfo>& mods() const { return mods_; }
  const cars::modmgr::Config& config() const { return cfg_; }
  const cars::modmgr::Resolution& resolution() const { return res_; }
  const cars::modmgr::Isolation& isolation() const { return iso_; }
  const cars::modmgr::UdlcStatus& udlc() const { return udlc_; }
  const cars::modmgr::ActivationReport& report() const { return report_; }
  const cars::plugins::Host& plugins() const { return *plugins_; }
  cars::modmgr::GameBuild build() const { return build_; }
  bool any_active() const { return !res_.active.empty(); }
  // Log lines for the console ("mod: ...").
  std::vector<std::string> log;
  // HUD notices from plugins (taken by the app).
  std::vector<std::pair<std::string, float>> notices;

  // For tests and the page: replaces the scanned state.
  void set_state(std::filesystem::path root, std::vector<cars::modmgr::ModInfo> mods, cars::modmgr::Config cfg,
                 cars::modmgr::GameBuild build);

 private:
  struct Tracked;
  std::filesystem::path root_, main_save_;
  std::vector<cars::modmgr::ModInfo> mods_;
  cars::modmgr::Config cfg_;
  cars::modmgr::Resolution res_;
  cars::modmgr::Isolation iso_;
  cars::modmgr::UdlcStatus udlc_;
  cars::modmgr::ActivationReport report_;
  cars::modmgr::GameBuild build_ = cars::modmgr::GameBuild::kAny;
  std::unique_ptr<cars::plugins::Host> plugins_;
  std::unique_ptr<Tracked> tracked_;
};

class ModsPage {
 public:
  void open(const ModsSession& session);
  void close() { open_ = false; }
  bool is_open() const { return open_; }
  // Returns true when the page wants the app to reload textures (X / F5).
  bool nav(Nav n);
  // Writes mods.toml (Y); false with `error`.
  bool save(std::string* error);

  struct Row {
    std::string name, title, version, author, status, summary;
    bool enabled = false, active = false, native = false, touches_save = false;
    std::string reason;
  };
  std::vector<Row> rows() const;
  int selected() const { return selected_; }
  bool dirty() const { return dirty_; }
  const cars::modmgr::Resolution& preview() const { return preview_; }
  const cars::modmgr::Config& config() const { return cfg_; }
  const std::vector<cars::modmgr::ModInfo>& mods() const { return mods_; }
  const cars::modmgr::ModInfo* selected_mod() const;
  // The save the next start plays ("YOUR SAVE", "MODDED COPY", ...).
  std::string save_label() const;
  std::string save_detail() const;
  bool save_isolated() const { return !preview_.save_profile.empty(); }
  const std::string& notice() const { return notice_; }
  void set_notice(std::string n) { notice_ = std::move(n); }
  std::string udlc_line;  // "UDLC v1.2 MOUNTED", filled from the session
  std::string textures_line;

 private:
  void refresh();
  std::filesystem::path root_;
  std::vector<cars::modmgr::ModInfo> mods_;
  cars::modmgr::Config cfg_;
  cars::modmgr::GameBuild build_ = cars::modmgr::GameBuild::kAny;
  cars::modmgr::Resolution preview_;
  std::string running_profile_;
  int selected_ = 0;
  bool open_ = false, dirty_ = false;
  std::string notice_;
};

void draw_mods_page(ui::List& l, float screen_w, float screen_h, const ModsPage& page);

// The world-texture loader's hook (showroom_main's streaming worker):
// forwards to the session the app registered (none: no change).
void set_world_texture_session(ModsSession* session);
void world_texture_hook(uint32_t record, bool srgb, cars::bake::Texture& texture);

// Extends `baked_ids` dynamically with cars added by active SQL/merge DB
// patches (or mod bake/cars/*.physics.txt files). Returns how many car IDs
// were added.
size_t extend_baked_catalog(std::map<int64_t, std::string>& baked_ids, const cars::PartsDb* db,
                            const ModsSession& session);

// Word-wraps `text` to lines no wider than `width` in `style`.
std::vector<std::string> wrap_text(const std::string& text, const ui::TextStyle& style, float width);

}  // namespace carview
