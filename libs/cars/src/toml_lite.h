// A small TOML subset for mod manifests, mods.toml and string patches:
// comments, [table] and [a.b] headers, key = value with basic and literal
// strings, integers, booleans and arrays of strings (which may span
// lines). Internal to shift_cars.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cars::toml {

struct Value {
  enum Kind { kString, kInt, kBool, kArray } kind = kString;
  std::string s;
  int64_t i = 0;
  bool b = false;
  std::vector<std::string> arr;
};

struct Item {
  std::string table;  // "" at the top, else the header ("options.strength")
  std::string key;
  Value value;
  int line = 0;
};

struct Doc {
  std::vector<Item> items;
  std::vector<std::string> errors;  // "line N: ..."
};

Doc parse(std::string_view text);
// A TOML basic string with its quotes.
std::string quote(std::string_view s);
// An array of basic strings.
std::string quote_list(const std::vector<std::string>& v);

}  // namespace cars::toml
