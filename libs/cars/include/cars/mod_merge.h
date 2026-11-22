// Archive member, XML/INI and string table mods (ours, Slice 13).
//
// From the active mods (the winner first):
//   members/<archive>/<member>      replaces the member (or adds one the
//                                   archive lacks) for every reader of
//                                   fh1::Archive (car, world, UI, physics)
//   xml_patches/<archive>/<member>  merged into the member's XML or INI
//   (alias merge/)                  bytes after it is read, every mod in
//                                   reverse load order (the winner last)
//   stringtables/<LANG|ALL>/<Table>.toml   IDS_ names and their text,
//   (alias strings/)                put over the loaded string tables
//
// <archive> is the archive's file name ("physics.zip") or a path ending in
// it ("tracks/colorado/bin.zip"), matched case-insensitively against the
// end of the archive's path; <member> is the member's path inside it.
//
// The XML merge walks both trees: an element of the patch matches the
// base's element with the same tag and the same key attribute (name, id,
// key or type, the first the patch element has), or without one the same
// tag at the same position among its siblings. A match takes the patch's
// attributes and merges children; an unmatched element (or one marked
// shift-add="true" or pinyon-add="true") is appended. Text content
// replaces the base's. The INI merge sets [section] key = value lines,
// adding sections and keys the base lacks.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fh1 {
struct StringTable;
class StringTables;
}  // namespace fh1

namespace cars::mod_merge {

bool merge_xml(std::string& base, std::string_view patch, std::string* error);
void merge_ini(std::string& base, std::string_view patch);

// `IDS_Name = "text"` lines (TOML strings with \n, \t, \", \\ and \uXXXX).
bool parse_string_patch(std::string_view text, std::vector<std::pair<std::string, std::string>>& out,
                        std::string* error);
// Puts the entries into `table` by the hash of their IDS_ names (replacing
// or adding, keys kept ascending). Returns the count put.
size_t apply_string_patch(fh1::StringTable& table, const std::vector<std::pair<std::string, std::string>>& entries);

struct Counts {
  size_t members = 0, xml = 0, strings = 0;
};
// Builds the registry from the mods' folders (the winner first) and
// installs the fh1::Archive override; an empty list removes it.
Counts set_active(const std::vector<std::filesystem::path>& mod_dirs_winner_first);

// The member's bytes as mods make them: a replacement, else `base` with the
// XML/INI patches merged. False when no mod touches the member.
bool resolve_member(const std::filesystem::path& archive, std::string_view member, std::vector<uint8_t>& data,
                    bool have_base);

// Applies the active string patches for `language` (ALL first, then the
// language's) to `tables`; returns the strings put.
size_t apply_string_patches(fh1::StringTables& tables, const std::string& language);

}  // namespace cars::mod_merge
