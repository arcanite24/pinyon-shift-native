// car-corpus: reproduces FORMAT-CARS's Tests counts over the player's files.
//
//   car-corpus --game-root <dir> [--state-root <dir>] [--dlc-extracted <dir>]
//              [--threads N] [--check]
//
// <game>: the folder holding the disc's media/ (or media itself).
// <state root>: the launcher's state root (title-update-v4/media.zip and the
// installed packages under user/0000000000000000/4D5309C9/00000002/).
// --dlc-extracted reads extracted offer ZIPs (<package>/<offer stem>/media/...)
// instead of the installed ZIPs. Without --game-root the SHIFT_GAME_ROOT and
// SHIFT_STATE_ROOT environment variables are read; with neither the program
// exits 77 (skipped, for ctest). --check compares every count with the
// spec's table and exits 1 on any difference. Prints counts and names only.
//
// Spec: FORMAT-CARS (Tests).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cars/database.h"
#include "cars/sources.h"
#include "cars/util.h"
#include "fh1data/carbin.h"
#include "fh1data/xds.h"

namespace {

namespace fs = std::filesystem;

struct Stats {
  std::map<std::string, uint64_t> n;
  std::array<uint64_t, 6> lods{};
  std::set<std::string> techniques;
  std::vector<std::string> problems;
  void add(const Stats& o) {
    for (const auto& [k, v] : o.n) n[k] += v;
    for (size_t i = 0; i < 6; ++i) lods[i] += o.lods[i];
    techniques.insert(o.techniques.begin(), o.techniques.end());
    problems.insert(problems.end(), o.problems.begin(), o.problems.end());
  }
};

std::string model_kind(const std::string& name) {
  if (name.find("caliper") != std::string::npos) return "caliper";
  if (name.find("rotor") != std::string::npos) return "rotor";
  if (name.size() > 15 && name.compare(name.size() - 15, 15, "_cockpit.carbin") == 0) return "cockpit";
  if (name.size() > 12 && name.compare(name.size() - 12, 12, "_lod0.carbin") == 0) return "lod0";
  return "lod";
}

bool ends_with(const std::string& s, const std::string& t) {
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

void census_model(const std::string& where, const std::vector<uint8_t>& data, Stats& s) {
  ++s.n["models"];
  const auto found = fh1::carbin_scan_part_count(data);
  if (found.size() == 1) ++s.n["draw-list scan finds one position"];
  fh1::Carbin model;
  std::string error;
  if (!fh1::parse_carbin(data, model, &error)) {
    s.problems.push_back(where + ": " + error);
    return;
  }
  ++s.n["models parsed"];
  if (fh1::encode_carbin(model) == data) ++s.n["models that write back byte for byte"];
  else s.problems.push_back(where + ": does not write back byte for byte");
  if (model.parts.empty()) {
    ++s.n["models with no parts"];
    return;
  }
  ++s.n["models with parts"];
  s.n["parts"] += model.parts.size();
  for (const auto& part : model.parts) {
    if (part.lod_pool.count >= 0)
      s.n["vertices in all pools"] += uint64_t(part.lod_pool.count) + part.lod0_pool.vertex_count();
    for (const auto& sub : part.subsections) {
      ++s.n["sub-sections"];
      if (sub.lod >= 0 && sub.lod < 6) ++s.lods[size_t(sub.lod)];
      if (sub.index_type == 4) ++s.n["triangle lists (type 4)"];
      if (sub.index_size == 4) ++s.n["32-bit index buffers"];
      s.techniques.insert(sub.material);
    }
  }
  if (fh1::carbin_draw_lists_cover(model)) ++s.n["draw lists cover every sub-section once"];
  else s.problems.push_back(where + ": draw lists do not cover the sub-sections");
  if (const size_t bad = fh1::carbin_bad_indices(model))
    s.problems.push_back(where + ": " + std::to_string(bad) + " indices outside their pool");
}

void census_xds(const std::string& where, const std::vector<uint8_t>& data, Stats& s) {
  ++s.n["xds"];
  fh1::XdsHeader h;
  std::string error;
  if (!fh1::parse_xds_header(data, h, &error)) {
    s.problems.push_back(where + ": " + error);
    return;
  }
  ++s.n["xds format " + std::to_string(h.format)];
  if (h.dimension == fh1::XdsDimension::k2D) ++s.n["xds 2D"];
  if (!h.tiled) {
    ++s.n["xds linear"];
    return;
  }
  const size_t layout = fh1::xds_layout_bytes(h);
  if (layout && layout == data.size() - 0x34) ++s.n["tiled xds whose payload equals the layout"];
  else s.problems.push_back(where + ": payload " + std::to_string(data.size() - 0x34) + ", layout " +
                            std::to_string(layout));
}

struct Item {
  cars::FolderRef ref;
  bool dlc = false;
  bool models = false;  // census the folder's top-level .carbin models
  bool textures = false;  // census every .xds in it
};

void run_item(const Item& item, Stats& s) {
  cars::Folder folder;
  std::string error;
  if (!folder.open(item.ref, &error)) {
    s.problems.push_back(item.ref.path.string() + ": " + error);
    return;
  }
  if (item.models) ++s.n["cars"];
  for (const std::string& name : folder.names()) {
    const bool model = item.models && name.find('/') == std::string::npos && ends_with(name, ".carbin") &&
                       name.rfind("stripped_", 0) != 0;
    const bool texture = item.textures && ends_with(name, ".xds");
    if (!model && !texture) continue;
    std::vector<uint8_t> data;
    if (!folder.read(name, data, &error)) {
      s.problems.push_back(error);
      continue;
    }
    const std::string where = item.ref.name + "/" + name;
    if (model) {
      ++s.n["kind " + model_kind(name)];
      census_model(where, data, s);
    }
    if (texture) census_xds(where, data, s);
  }
}

struct Expect {
  const char* key;
  uint64_t base, dlc;
};

}  // namespace

int main(int argc, char** argv) {
  fs::path game, state, dlc_extracted;
  unsigned threads = 0;
  bool check = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game-root") game = next();
    else if (a == "--state-root") state = next();
    else if (a == "--dlc-extracted") dlc_extracted = next();
    else if (a == "--threads") threads = unsigned(std::stoul(next()));
    else if (a == "--check") check = true;
    else {
      std::fprintf(stderr, "usage: car-corpus --game-root <dir> [--state-root <dir>] "
                           "[--dlc-extracted <dir>] [--threads N] [--check]\n");
      return 2;
    }
  }
  if (game.empty()) {
    if (const char* g = std::getenv("SHIFT_GAME_ROOT")) game = g;
    if (const char* s = std::getenv("SHIFT_STATE_ROOT"); s && state.empty()) state = s;
    if (const char* d = std::getenv("SHIFT_DLC_EXTRACTED"); d && dlc_extracted.empty()) dlc_extracted = d;
  }
  if (game.empty()) {
    std::printf("car-corpus: no game root (--game-root or SHIFT_GAME_ROOT): skipped\n");
    return 77;
  }
  const auto start = std::chrono::steady_clock::now();
  cars::Install install;
  std::string error;
  if (!cars::discover_install(game, state, dlc_extracted, install, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  std::vector<Item> items;
  static const std::set<std::string> kNotCars = {"shared", "driver", "appearancepresets"};
  for (const auto& [media, ref] : install.base_cars)
    items.push_back({ref, false, !kNotCars.count(media), true});
  for (const auto& [media, ref] : install.base_wheels) items.push_back({ref, false, false, true});
  std::set<std::string> seen;
  for (const auto& [media, _] : install.base_cars) seen.insert(media);
  for (const cars::Package& p : install.packages) {
    for (const auto& archive : p.archives) {
      cars::FolderRef ref;
      ref.kind = fs::is_directory(archive) ? cars::FolderRef::Kind::kDirectory : cars::FolderRef::Kind::kArchive;
      ref.path = archive;
      ref.origin = p.id;
      ref.name = p.id + "/" + archive.filename().string();
      items.push_back({ref, true, false, true});
    }
    for (const auto& [media, ref] : p.cars)
      if (seen.insert(media).second) items.push_back({ref, true, true, false});
  }

  std::vector<Stats> results(items.size());
  cars::parallel_for(items.size(), threads, [&](size_t i) { run_item(items[i], results[i]); });
  Stats base, dlc;
  for (size_t i = 0; i < items.size(); ++i) (items[i].dlc ? dlc : base).add(results[i]);

  // Database rows: every merged Data_Car row with a car folder, a wheel
  // folder, a colour row and wheel rows 5-8.
  cars::Database db;
  uint64_t rows = 0, complete = 0;
  std::set<std::string> media_names;
  std::vector<std::string> db_problems;
  if (!cars::load_database(install, db, &error)) {
    db_problems.push_back("database: " + error);
  } else {
    for (const auto& r : db.cars) {
      ++rows;
      media_names.insert(cars::lower(r.media_name));
      const bool folder = install.find_car(r.media_name).has_value();
      const bool wheel = !r.wheel_media.empty() && install.find_wheel(r.wheel_media).has_value();
      const bool colour = !r.colours.empty();
      const bool wheels = r.has_part_position[5] && r.has_part_position[6] && r.has_part_position[7] &&
                          r.has_part_position[8];
      if (folder && wheel && colour && wheels) ++complete;
      else db_problems.push_back("car " + std::to_string(r.id) + " " + r.media_name + ":" +
                                 (folder ? "" : " no folder") + (wheel ? "" : " no wheel folder") +
                                 (colour ? "" : " no colour") + (wheels ? "" : " no wheel rows"));
    }
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::set<std::string> all_techniques = base.techniques;
  all_techniques.insert(dlc.techniques.begin(), dlc.techniques.end());
  auto get = [](const Stats& s, const char* key) {
    auto it = s.n.find(key);
    return it == s.n.end() ? uint64_t(0) : it->second;
  };
  for (const auto& [label, s] : {std::pair<const char*, const Stats*>{"base", &base}, {"dlc-only", &dlc}}) {
    std::printf("[%s]\n", label);
    for (const auto& [k, v] : s->n) std::printf("  %s: %llu\n", k.c_str(), (unsigned long long)v);
    std::printf("  sub-sections by LOD 0-5: %llu / %llu / %llu / %llu / %llu / %llu\n",
                (unsigned long long)s->lods[0], (unsigned long long)s->lods[1],
                (unsigned long long)s->lods[2], (unsigned long long)s->lods[3],
                (unsigned long long)s->lods[4], (unsigned long long)s->lods[5]);
    std::printf("  distinct techniques: %zu\n", s->techniques.size());
  }
  std::printf("distinct techniques over both: %zu\n", all_techniques.size());
  std::printf("[database] packages %zu, merges %zu, Data_Car rows %llu (base %zu, v4 %zu), distinct media "
              "names %zu, complete rows %llu\n",
              install.packages.size(), db.merges_applied.size(), (unsigned long long)rows, db.base_rows,
              db.v4_rows, media_names.size(), (unsigned long long)complete);

  // Loose brake models.
  uint64_t brakes_ok = 0, brakes_empty = 0;
  {
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(install.media / "brakes", ec))
      if (e.is_regular_file() && cars::lower(e.path().extension().string()) == ".carbin") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      std::vector<uint8_t> data;
      cars::Folder dir;
      cars::FolderRef ref{cars::FolderRef::Kind::kDirectory, f.parent_path(), "", "base", "brakes"};
      if (!dir.open(ref, &error) || !dir.read(f.filename().string(), data, &error)) continue;
      fh1::Carbin model;
      if (fh1::parse_carbin(data, model, &error) && fh1::encode_carbin(model) == data) {
        if (model.parts.empty()) ++brakes_empty;
        else ++brakes_ok;
      } else {
        base.problems.push_back("brakes/" + f.filename().string() + ": " + error);
      }
    }
  }
  std::printf("[brakes] parse and write back with parts: %llu, empty models: %llu\n",
              (unsigned long long)brakes_ok, (unsigned long long)brakes_empty);
  std::printf("time: %.1f s\n", seconds);

  for (const auto& p : base.problems) std::fprintf(stderr, "problem (base): %s\n", p.c_str());
  for (const auto& p : dlc.problems) std::fprintf(stderr, "problem (dlc): %s\n", p.c_str());
  for (const auto& p : db_problems) std::fprintf(stderr, "problem (database): %s\n", p.c_str());

  int failures = int(base.problems.size() + dlc.problems.size() + db_problems.size());
  if (check) {
    static const Expect kExpected[] = {
        {"cars", 176, 64},
        {"models", 1883, 704},
        {"kind lod", 176, 64},
        {"kind lod0", 174, 64},
        {"kind cockpit", 173, 64},
        {"kind caliper", 680, 256},
        {"kind rotor", 680, 256},
        {"models with no parts", 1, 0},
        {"models that write back byte for byte", 1883, 704},
        {"parts", 14917, 6074},
        {"sub-sections", 118629, 53211},
        {"triangle lists (type 4)", 11, 7},
        {"32-bit index buffers", 28359, 13008},
        {"vertices in all pools", 35193432, 16461551},
        {"draw-list scan finds one position", 1883, 704},
        {"draw lists cover every sub-section once", 1882, 704},
        {"xds", 4794, 2647},
        {"tiled xds whose payload equals the layout", 4793, 2647},
    };
    auto expect = [&](const std::string& what, uint64_t got, uint64_t want) {
      const bool ok = got == want;
      std::printf("%s %s: %llu (spec %llu)\n", ok ? "ok  " : "FAIL", what.c_str(), (unsigned long long)got,
                  (unsigned long long)want);
      if (!ok) ++failures;
    };
    for (const Expect& e : kExpected) {
      expect(std::string("base ") + e.key, get(base, e.key), e.base);
      expect(std::string("dlc-only ") + e.key, get(dlc, e.key), e.dlc);
    }
    static const uint64_t kBaseLods[6] = {30390, 20924, 19954, 19278, 17312, 10771};
    static const uint64_t kDlcLods[6] = {13844, 9416, 9033, 8645, 7772, 4501};
    for (int l = 0; l < 6; ++l) {
      expect("base sub-sections at LOD " + std::to_string(l), base.lods[size_t(l)], kBaseLods[l]);
      expect("dlc-only sub-sections at LOD " + std::to_string(l), dlc.lods[size_t(l)], kDlcLods[l]);
    }
    expect("base distinct techniques", base.techniques.size(), 266);
    expect("dlc-only distinct techniques", dlc.techniques.size(), 161);
    expect("distinct techniques over both", all_techniques.size(), 284);
    expect("loose brake models that parse", brakes_ok, 118);
    expect("loose brake models that are empty", brakes_empty, 2);
    expect("merged Data_Car rows", rows, 250);
    expect("merged rows with a car folder, wheel folder, colour and wheel rows", complete, 250);
    expect("distinct media names", media_names.size(), 240);
  }
  return failures ? 1 : 0;
}
