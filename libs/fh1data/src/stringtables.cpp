#include "fh1data/stringtables.h"

#include <algorithm>
#include <cctype>

namespace fh1 {
namespace {

uint32_t be32(std::span<const uint8_t> b, size_t at) {
  return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
}
uint16_t be16(std::span<const uint8_t> b, size_t at) { return uint16_t(b[at] << 8 | b[at + 1]); }

bool fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

}  // namespace

uint16_t string_hash(std::string_view name) {
  uint32_t h = 0xFFFF;
  for (char c : name) {
    h ^= uint8_t(std::tolower(static_cast<unsigned char>(c)));
    h = ((h << 7) | (h >> 9)) & 0xFFFF;
  }
  return uint16_t(h);
}

const std::u16string* StringTable::find(uint16_t key) const {
  auto it = std::lower_bound(keys.begin(), keys.end(), key);
  if (it == keys.end() || *it != key) return nullptr;
  return &text[size_t(it - keys.begin())];
}

bool parse_string_table(std::span<const uint8_t> b, StringTable& out, std::string* error) {
  out = StringTable{};
  if (b.size() < 20 || b[0] != 'L' || b[1] != 'S' || b[2] != 'B' || b[3] != '2') return fail(error, "not LSB2");
  const size_t s = be32(b, 12);
  if (s + 8 > b.size()) return fail(error, "strings chunk out of range");
  const size_t count = be32(b, s + 4);
  const size_t entries = s + 8;
  const size_t pool = entries + 6 * (count + 1);
  if (count > 0x10000 || pool > b.size()) return fail(error, "strings chunk entries out of range");
  if (be16(b, entries + 6 * count) != 0xFFFF) return fail(error, "no 0xFFFF sentinel");
  out.keys.reserve(count);
  out.text.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const uint16_t key = be16(b, entries + 6 * i);
    if (key == 0xFFFF || (i && key <= out.keys.back())) return fail(error, "keys not strictly ascending");
    size_t at = pool + 2 * size_t(be32(b, entries + 6 * i + 2));
    std::u16string t;
    for (;; at += 2) {
      if (at + 2 > b.size()) return fail(error, "string runs past the end");
      const uint16_t u = be16(b, at);
      if (!u) break;
      t.push_back(char16_t(u));
    }
    out.keys.push_back(key);
    out.text.push_back(std::move(t));
  }
  return true;
}

std::string utf16_to_utf8(std::u16string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
      c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t(s[++i]) - 0xDC00);
    else if (c >= 0xD800 && c < 0xE000)
      c = 0xFFFD;  // an unpaired surrogate
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F)),
          out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

void StringTables::put(std::string_view name, StringTable table) {
  if (name.size() > 4) {
    std::string tail(name.substr(name.size() - 4));
    for (char& c : tail) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (tail == ".str") name.remove_suffix(4);
  }
  tables_[string_hash(name)] = std::move(table);
}

bool StringTables::is_reference(std::string_view s) {
  if (s.size() < 3 || s[0] != '_' || s[1] != '&') return false;
  return std::all_of(s.begin() + 2, s.end(), [](char c) { return c >= '0' && c <= '9'; }) && s.size() <= 12;
}

std::optional<std::string> StringTables::resolve(std::string_view ref) const {
  if (ref.size() >= 2 && ref[0] == '_' && ref[1] == '&') ref.remove_prefix(2);
  if (ref.empty() || ref.size() > 10) return std::nullopt;
  uint64_t n = 0;
  for (char c : ref) {
    if (c < '0' || c > '9') return std::nullopt;
    n = n * 10 + uint64_t(c - '0');
  }
  if (n > 0xFFFFFFFFull) return std::nullopt;
  auto t = tables_.find(uint16_t(n >> 16));
  if (t == tables_.end()) return std::nullopt;
  const std::u16string* s = t->second.find(uint16_t(n & 0xFFFF));
  if (!s) return std::nullopt;
  return utf16_to_utf8(*s);
}

}  // namespace fh1
