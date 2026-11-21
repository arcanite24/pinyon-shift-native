// profile-import: reads a plaintext export of the player's profile
// (FORMAT-PROFILE) and prints what M6 imports.
//
//   profile-import <export folder>         prints the summary
//   profile-import --check <exports root>  checks each export folder under
//                                          the root (user, user-modded)
//                                          against its summary.json; exit
//                                          77 when there are none
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "profile/profile.h"

namespace {

// summary.json's top-level numbers, booleans and number lists (a list's
// entries as key_N); strings and objects are skipped.
std::map<std::string, int64_t> read_expected(const std::filesystem::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string s = ss.str();
  std::map<std::string, int64_t> out;
  size_t at = 0;
  int depth = 0;
  while (at < s.size()) {
    const char ch = s[at];
    if (ch == '{' || ch == '[') ++depth;
    if (ch == '}' || ch == ']') --depth;
    if (ch != '"' || depth != 1) {
      ++at;
      continue;
    }
    const size_t end = s.find('"', at + 1);
    const std::string key = s.substr(at + 1, end - at - 1);
    size_t v = s.find_first_not_of(" \t\r\n", s.find(':', end) + 1);
    at = v;
    if (s.compare(v, 4, "true") == 0) out[key] = 1, at += 4;
    else if (s.compare(v, 5, "false") == 0) out[key] = 0, at += 5;
    else if (s[v] == '-' || (s[v] >= '0' && s[v] <= '9')) out[key] = std::strtoll(s.c_str() + v, nullptr, 10);
    else if (s[v] == '[') {
      const size_t close = s.find(']', v);
      std::stringstream list(s.substr(v + 1, close - v - 1));
      std::string item;
      int k = 0;
      bool numbers = true;
      std::map<std::string, int64_t> items;
      while (std::getline(list, item, ',')) {
        const size_t d = item.find_first_not_of(" \t\r\n");
        if (d == std::string::npos) continue;
        if (!(item[d] == '-' || (item[d] >= '0' && item[d] <= '9'))) numbers = false;
        items[key + "_" + std::to_string(k++)] = std::strtoll(item.c_str() + d, nullptr, 10);
      }
      if (numbers) {
        out[key] = k;
        out.insert(items.begin(), items.end());
      }
      at = close + 1;
    }
  }
  return out;
}

void print(const profile::Imported& p) {
  for (const auto& [k, v] : profile::summary(p)) std::printf("%-32s %lld\n", k.c_str(), static_cast<long long>(v));
  std::printf("%-32s %lld\n", "credits", static_cast<long long>(p.credits));
  std::printf("%-32s %lld\n", "xp", static_cast<long long>(p.xp));
  std::printf("%-32s %lld\n", "level", static_cast<long long>(p.level));
  if (const auto* car = p.selected_car())
    std::printf("%-32s %lld (car %lld)\n", "career_car", static_cast<long long>(car->id), static_cast<long long>(car->car_id));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--check") {
    const std::filesystem::path root = argv[2];
    int checked = 0, bad = 0;
    for (const char* tree : {"user", "user-modded"}) {
      const auto folder = root / tree;
      if (!std::filesystem::exists(folder / "summary.json")) continue;
      profile::Imported p;
      std::string error;
      if (!profile::import_profile(folder, p, &error)) {
        std::printf("%s: %s\n", tree, error.c_str());
        ++bad, ++checked;
        continue;
      }
      const auto ours = profile::summary(p);
      int compared = 0, wrong = 0;
      for (const auto& [k, v] : read_expected(folder / "summary.json")) {
        const auto it = ours.find(k);
        if (it == ours.end()) continue;  // a count the import does not keep
        ++compared;
        if (it->second != v) {
          std::printf("%s: %s is %lld, the export says %lld\n", tree, k.c_str(), static_cast<long long>(it->second),
                      static_cast<long long>(v));
          ++wrong;
        }
      }
      std::printf("%s: %d counts, %d wrong\n", tree, compared, wrong);
      ++checked;
      bad += wrong != 0 || compared == 0;
    }
    if (checked == 0) {
      std::printf("no exports under %s (skipped)\n", root.string().c_str());
      return 77;
    }
    return bad ? 1 : 0;
  }
  if (argc != 2) {
    std::fprintf(stderr, "usage: profile-import <export folder> | --check <exports root>\n");
    return 2;
  }
  profile::Imported p;
  std::string error;
  if (!profile::import_profile(argv[1], p, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  print(p);
  return 0;
}
