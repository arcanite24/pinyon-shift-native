// FH1 string tables (`<Table>.str`, big-endian LSB2) and the database's
// `_&N` text references: N's high 16 bits are the hash of the table's
// name, its low 16 bits the hash of the string's IDS_ name, which the
// table stores as the key. Text is UTF-16BE; resolve() returns UTF-8.
// Spec: FORMAT-STRINGTABLES (docs/formats/STRINGTABLES.md) in horizon1-recomp.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh1 {

// H: 0xFFFF, then for each lower-cased byte XOR it in and rotate the 16
// bits left by 7.
uint16_t string_hash(std::string_view name);

// One table's strings chunk: key -> UTF-16 code units (no terminator).
struct StringTable {
  std::vector<uint16_t> keys;                // ascending
  std::vector<std::u16string> text;          // parallel to keys
  const std::u16string* find(uint16_t key) const;
};

// Parses a .str file. Rejects a bad magic, a strings chunk out of range,
// keys not strictly ascending or a missing 0xFFFF sentinel.
bool parse_string_table(std::span<const uint8_t> bytes, StringTable& out, std::string* error);

std::string utf16_to_utf8(std::u16string_view s);

class StringTables {
 public:
  // Adds or replaces (whole table) the table named `name` (".str" optional).
  void put(std::string_view name, StringTable table);
  size_t size() const { return tables_.size(); }

  // `_&N` (or a bare N) -> the text in UTF-8; nullopt when the table or the
  // key is missing, or `ref` is not a reference.
  std::optional<std::string> resolve(std::string_view ref) const;
  // Whether `s` has the reference's shape.
  static bool is_reference(std::string_view s);

 private:
  std::unordered_map<uint16_t, StringTable> tables_;
};

}  // namespace fh1
