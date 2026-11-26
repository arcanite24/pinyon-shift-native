// The player's settings (ours, M7): settings.txt in the user folder, one
// option per line as "key value" or "key" (the drive's own options without
// the dashes: "preset low", "no-traffic", "difficulty hard"). They go in
// before the command line, so the command line wins. Only the options
// listed in settings.cpp are kept; others are noted and skipped. The window
// reads them; headless runs do not (the gates stay as given) unless
// SHIFT_SETTINGS names the file.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace carview {

// SHIFT_SETTINGS, else <user folder>/settings.txt (SDL's pref path
// shift-engine/), or empty.
std::filesystem::path settings_path();
// The user data folder (SDL's pref path shift-engine/), or empty.
std::filesystem::path default_user_dir();
// The portable folder's data/ when portable.txt sits beside the executable
// (platform_support.h), else empty.
std::filesystem::path portable_user_dir();
// The user folder's mods/ (the default mods root), or empty.
std::filesystem::path default_mods_root();
// The user folder's photos/, replays/, and achievements.txt paths, or empty.
std::filesystem::path default_photos_dir();
std::filesystem::path default_replays_dir();
std::filesystem::path default_achievements_path();

// --no-settings: the run neither reads nor writes any settings file
// (read_settings returns nothing, write_settings refuses), so a test or
// tool run never changes the player's settings.txt.
void set_settings_file_disabled(bool disabled);
bool settings_file_disabled();

// The file's options as command-line tokens ("--preset", "low", ...).
// Missing file (or settings_file_disabled()): none.
std::vector<std::string> read_settings(const std::filesystem::path& file, std::vector<std::string>* notes);

// Writes the kept options of `args` (command-line tokens, settings first),
// the last of each kept: --save-settings. Refuses (false) when
// settings_file_disabled().
bool write_settings(const std::filesystem::path& file, const std::vector<std::string>& args, std::string* error);

// Returns true when `arg` (with leading "--") is a recognized settings.txt
// option. When non-null, `takes_value` is set to true if it consumes a value.
bool is_settings_option(const std::string& arg, bool* takes_value = nullptr);

}  // namespace carview
