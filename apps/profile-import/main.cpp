// profile-import: reads a plaintext export of the player's profile
// (FORMAT-PROFILE) and prints what M6 imports.
//
//   profile-import <export folder>         prints the summary
//   profile-import --fields <folder>       prints the Main fields
//   profile-import --garage <folder> <game root> [state root]
//                                          checks each garage car the
//                                          database has: the mass rule
//                                          against CurbWeight, the class
//                                          of the stored index, PartsValue
//   profile-import --check <exports root>  checks each export folder under
//                                          the root (user, user-modded)
//                                          against its summary.json; exit
//                                          77 when there are none
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "profile/drive_save.h"
#include "profile/profile.h"
#include "profile/xenia_import.h"
#include "cars/sources.h"

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

std::filesystem::path default_user_dir() {
#if defined(_WIN32)
  if (const char* app = std::getenv("APPDATA"); app && *app) return std::filesystem::path(app) / "shift-engine";
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "shift-engine";
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / ".local" / "share" / "shift-engine";
#endif
  return std::filesystem::current_path() / "shift-user";
}

std::map<int64_t, std::string> scan_baked_cars(const std::filesystem::path& bake_dir) {
  std::map<int64_t, std::string> out;
  std::error_code ec;
  if (bake_dir.empty() || !std::filesystem::is_directory(bake_dir / "cars", ec)) return out;
  for (const auto& f : std::filesystem::directory_iterator(bake_dir / "cars", ec)) {
    if (!f.path().string().ends_with(".physics.txt")) continue;
    std::ifstream in(f.path());
    for (std::string line; std::getline(in, line);) {
      if (line.rfind("car ", 0) != 0) continue;
      std::istringstream ls(line.substr(4));
      int64_t id = 0;
      std::string media;
      if (ls >> id >> media) out[id] = media;
      break;
    }
  }
  return out;
}

int install(int argc, char** argv) {
  const std::filesystem::path src = argv[2];
  std::filesystem::path save_dir, bake_dir, state_root;
  bool replace = false, v4_flag = false;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--save-dir") save_dir = next();
    else if (a == "--bake") bake_dir = next();
    else if (a == "--state-root") state_root = next();
    else if (a == "--replace") replace = true;
    else if (a == "--v4") v4_flag = true;
    else {
      std::fprintf(stderr, "profile-import --install: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  const std::filesystem::path user_dir = default_user_dir();
  if (save_dir.empty()) save_dir = user_dir / "drive";
  std::error_code ec;
  if (state_root.empty())
    if (const char* s = std::getenv("SHIFT_STATE_ROOT"); s && *s) state_root = s;
  const auto baked = scan_baked_cars(bake_dir);
  profile::import::Options opts;
  opts.save_dir = save_dir;
  opts.replace_existing = replace;
  opts.v4_available =
      v4_flag || (!state_root.empty() && std::filesystem::is_regular_file(state_root / "title-update-v4" / "media.zip", ec));
  opts.backups.save_dir = save_dir;
  opts.backups.backups_dir = save_dir.parent_path() / "backups" / "saves";
  if (!baked.empty()) {
    opts.media_of = [&](int64_t id) {
      auto it = baked.find(id);
      return it == baked.end() ? std::string() : it->second;
    };
  }
  const auto res = profile::import::run(src, opts);
  for (const auto& n : res.notes) std::printf("note: %s\n", n.c_str());
  if (!res.ok) {
    std::fprintf(stderr, "profile-import --install: %s\n", res.error.c_str());
    return 1;
  }
  std::printf("imported into %s: %lld credits, %zu garage cars, %zu signs, car %s (%s)\n", save_dir.string().c_str(),
              static_cast<long long>(res.credits), res.garage_cars, res.signs, res.car.c_str(),
              res.v4_save ? "v4" : "base");
  return 0;
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
  if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--garage") {
    profile::Imported p;
    std::string error;
    if (!profile::import_profile(argv[2], p, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    const garage::Garage g = profile::to_garage(p);
    cars::Install install;
    cars::PartsDb db;
    std::vector<garage::ClassBand> bands;
    if (!cars::discover_install(argv[3], argc == 5 ? argv[4] : "", "", install, &error) || !db.open(install, &error) ||
        !garage::load_class_bands(argv[3], bands, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    int checked = 0, mass_ok = 0, class_ok = 0, value_ok = 0, missing = 0;
    for (const auto& c : g.cars) {
      const auto* row = [&]() -> const profile::GarageCar* {
        for (const auto& r : p.garage)
          if (r.id == c.id) return &r;
        return nullptr;
      }();
      int64_t paid = 0;
      for (const auto& b : g.bought)
        if (b.garage_id == c.id) paid += b.price_paid;
      value_ok += paid == c.parts_value;
      class_ok += garage::class_of(c.performance_index, bands) == c.class_id;
      cars::CarPhysics phys;
      std::string e;
      if (!db.physics(c.car_id, garage::fitted(c), phys, &e)) {
        ++missing;
        continue;
      }
      ++checked;
      const double mass = phys.get("List_UpgradeCarBodyWeight.Mass", 0);
      const double stored = row && row->reals.count("CurbWeight") ? row->reals.at("CurbWeight") * 100 : 0;
      const bool ok = std::abs(mass - stored) < 0.1;
      mass_ok += ok;
      std::printf("%3lld %-28s %s %4d  mass %7.1f stored %7.1f %s  parts %lld\n", (long long)c.id, phys.media_name.c_str(),
                  bands[size_t(std::clamp(c.class_id, 0, int(bands.size()) - 1))].name.c_str(),
                  garage::shown_index(c.performance_index, bands), mass, stored, ok ? "ok" : "OFF", (long long)c.parts_value);
    }
    {
      // The Autoshow list and a new car's stock slots (SPEC-AUTOSHOW-PAINT).
      std::vector<cars::DealerCar> dealer;
      std::map<std::string, int64_t> stock;
      std::vector<cars::PartRow> options;
      db.dealer(dealer, &error);
      db.stock_slots(1009, stock, &error);
      if (const auto* sel = g.selected_car()) db.options(sel->car_id, garage::fitted(*sel), options, &error);
      std::printf("dealer: %zu listable cars; car 1009: %zu stock slots; the selected car: %zu part rows\n",
                  dealer.size(), stock.size(), options.size());
    }
    std::printf("garage: %zu cars; class %d, parts value %d; mass rule %d of %d in the database (%d not in it)\n",
                g.cars.size(), class_ok, value_ok, mass_ok, checked, missing);
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--fields") {
    profile::Imported p;
    std::string error;
    if (!profile::import_profile(argv[2], p, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    if (const auto* m = profile::find(p.body.fields, "Main"))
      for (const auto& f : m->value.fields)
        if (f.value.type != profile::Value::kStruct)
          std::printf("%-36s %g\n", f.name.c_str(), f.value.as_double());
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "--install") return install(argc, argv);
  if (argc != 2) {
    std::fprintf(stderr,
                 "usage: profile-import <export folder> | --check <exports root>\n"
                 "       profile-import --install <export folder | export ZIP | Xenia/Pinyon folder>\n"
                 "                      [--save-dir DIR] [--bake DIR] [--state-root DIR] [--replace]\n");
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
