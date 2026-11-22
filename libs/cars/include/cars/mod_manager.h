// The mod manager (ours, Slice 13): structured mods over cars::mods.
//
// A mod is a folder <mods root>/<name>/ with a manifest, mod.toml (flat
// TOML) or the older mod.txt ("key value" lines), and any of:
//   bake/ sound/ media/ radio/ game/media/   files laid over the play folder
//                                           and the disc (cars/mods.h)
//   db/*.sql, db/*_merge.slt                game database patches (db_patch.h)
//   members/<archive>/<member>              archive member replacements
//   xml_patches/<archive>/<member>          XML / INI merges (alias merge/)
//   stringtables/<LANG|ALL>/<Table>.toml    string table patches (alias strings/)
//   textures/<16 hex>.png|dds|tga           replacement textures (texture_mods.h)
//   shaders/<name>.<stage>.spv              Vulkan shader overrides (shader_mods.h)
//   <library>                               a native plugin (shift_mod_api.h)
//   preview.png                             the Mods page's image
//
// mod.toml keys (all optional but name):
//   name = "my_mod"           title = "My Mod"      author = "..."
//   version = "1.0.0"         description = "..."   preview = "preview.png"
//   requires = ["a"]          conflicts = ["b"]     load_after = ["c"]
//   priority = 0              enabled = true        touches_save = true
//   profile = ""              library = "code/my_mod.dll"
//   game_version = "any"      hide_dlc = ["<package id>"]
//   [options.<key>]  label = "..."  choices = ["a", "b"]  default = 0
//
// The load order lists every mod, the winner first: <mods root>/mods.toml's
// `order`, then the rest by priority (highest first) and name. A mod that
// requires or loads after another is moved above it, so its files and
// patches win over the ones it builds on. A mod is active when it is
// enabled, every mod it requires is active, no active mod above it
// conflicts with it, and it is made for the running build.
//
// Save isolation: with any active mod whose `touches_save` is true, the
// game plays <save dir>/user-modded, copied from the player's own save the
// first time; a mod with a `profile` plays a new save in
// <save dir>/user-<profile>. The player's own save is never opened while
// such a mod is on. Mods with `touches_save = false` (looks and sound only)
// keep the player's save.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cars::modmgr {

enum class GameBuild { kAny, kBase, kV4 };

struct ModOption {
  std::string key, label;
  std::vector<std::string> choices;
  int def = 0;
};

enum class ModKind { kFolder, kXe };

struct Manifest {
  std::string name, title, author, version, description, preview;
  std::vector<std::string> dependencies, conflicts, load_after, hide_dlc;
  int priority = 0;
  bool enabled = true;       // the manifest's default; mods.toml overrides it
  bool touches_save = true;  // false: looks and sound only (keeps the player's save)
  std::string profile;       // "": the shared modded save; else <save dir>/user-<profile>
  std::string library;       // a native plugin, relative to the mod folder
  std::string game_version;  // "", "any", "base" or "v4"
  std::vector<ModOption> options;
  ModKind kind = ModKind::kFolder;
  std::string xe_version;  // kXe: "1.0" or "1.01"
};

// What a mod folder holds (file counts per kind).
struct Content {
  int bake = 0, sound = 0, media = 0, radio = 0, db = 0, members = 0, xml = 0, strings = 0, textures = 0,
      shaders = 0;
  bool native = false;
  int total() const { return bake + sound + media + radio + db + members + xml + strings + textures + shaders; }
  // A short list for the Mods page ("DB 2  TEX 14  XML 1").
  std::string summary() const;
};

struct ModInfo {
  std::string name;  // the folder's name
  std::filesystem::path dir;
  Manifest manifest;
  Content content;
  bool has_preview = false;
  std::vector<std::string> warnings;  // manifest lines that were not understood
};

// Parsers (exposed for tests). False on a line that is not `key = value`,
// a table header or a comment (the line is reported in `warnings`).
bool parse_manifest_toml(std::string_view text, Manifest& out, std::vector<std::string>* warnings = nullptr);
void parse_manifest_txt(std::string_view text, Manifest& out);

// The mods under `root` (folders with mod.toml, mod.txt or an XE install
// record), by name. Folders starting with '_' or '.' are skipped.
std::vector<ModInfo> scan(const std::filesystem::path& root);

// <mods root>/mods.toml: the player's choices.
struct Config {
  std::vector<std::string> order;  // the winner first
  std::map<std::string, bool> enabled;
  std::map<std::string, std::map<std::string, std::string>> options;  // mod -> key -> choice
  bool native_plugins = false;  // native code runs only when the player opted in
};
std::filesystem::path config_path(const std::filesystem::path& root);
bool read_config(const std::filesystem::path& file, Config& out);
bool write_config(const std::filesystem::path& file, const Config& cfg, std::string* error);

struct Entry {
  size_t mod = 0;  // index into the scanned mods
  bool enabled = false, active = false;
  std::string reason;  // why an enabled mod is not active ("needs a", "made for v4")
};

struct Resolution {
  std::vector<Entry> entries;  // every mod in load order, the winner first
  std::vector<size_t> active;  // active mods, the winner first
  std::vector<std::string> hidden_dlc;
  bool touches_save = false;
  std::string save_profile;  // "": the player's save; "modded"; or a mod's profile
  bool native_requested = false;  // an active mod has a library
  std::vector<std::string> notes;
};

Resolution resolve(const std::vector<ModInfo>& mods, const Config& cfg, GameBuild build);

// The folder a save profile plays: `main_save_dir` itself for "", else
// main_save_dir/user-<profile>.
std::filesystem::path save_dir_for(const std::filesystem::path& main_save_dir, const std::string& profile);

struct Isolation {
  std::filesystem::path save_dir;
  bool isolated = false;
  bool created = false;  // made now (copied from the player's save, or new)
  std::string profile;
};
// Picks (and on first use makes) the save folder for `r`: "modded" copies
// the player's files (not folders) from `main_save_dir`; a mod profile
// starts empty. Writes mods.lock (the active mods and their hashes) into an
// isolated folder. The player's own files are only read.
Isolation isolate_save(const std::filesystem::path& main_save_dir, const std::vector<ModInfo>& mods,
                       const Resolution& r, std::string* error);

// A stable hash of a mod's manifest and file list (names and sizes).
uint64_t mod_hash(const ModInfo& m);

// ---- UDLC v1.2 and the Traffic Car Unlocker (community DLC packages) ----
inline constexpr const char* kUdlcPackage = "4D5309C900000729";
inline constexpr const char* kUdlcTrafficPackage = "4D5309C900000732";
inline constexpr const char* kRallyPackage = "6F6992766050D818245ADD408031E280FB5F4E634D";

struct UdlcStatus {
  bool installed = false, header = false, enabled = false, record = false;
  std::string version;  // the import record's ("1.2"), or "" without one
  bool traffic_installed = false, traffic_header = false, traffic_enabled = false;
  std::string problem;  // why it cannot mount ("" when it can)
  bool mountable() const { return installed && header && enabled && problem.empty(); }
  bool traffic_mountable() const { return mountable() && traffic_installed && traffic_header && traffic_enabled; }
};
// Reads the state root's content folders and dlc/<package>.json records.
UdlcStatus detect_udlc(const std::filesystem::path& state_root);

// ---- What discover_install consults (sources.cpp) ----
// Whether a package is hidden: by an active mod's hide_dlc, or UDLC / its
// unlocker while UDLC is not mounted.
bool package_hidden(const std::string& package_id);
void set_hidden_packages(std::vector<std::string> ids);
void set_udlc_mount(bool udlc, bool traffic);
bool udlc_mounted();

// ---- Activation ----
struct ActivateOptions {
  std::filesystem::path bake_dir, sound_dir, game_root;
  bool udlc = false, udlc_traffic = false;  // mount UDLC (detect_udlc said it can)
};
struct ActivationReport {
  size_t db_patches = 0, members = 0, xml_patches = 0, string_patches = 0, textures = 0, shaders = 0;
  std::vector<std::string> notes;
};
// Lays the active mods over the play folder (cars::mods), registers their
// database, member, XML and string patches, textures and shaders, hides
// their DLC and mounts UDLC as asked. Call once at start-up, before the
// databases and archives are read; calling it again replaces everything.
ActivationReport activate(const std::vector<ModInfo>& mods, const Resolution& r, const ActivateOptions& opts);

// The text forms used by the Mods page and the logs.
const char* build_name(GameBuild b);

}  // namespace cars::modmgr
