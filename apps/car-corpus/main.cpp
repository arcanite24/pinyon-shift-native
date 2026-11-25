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
#include <array>
#include <cmath>
#include <cstring>
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

// The stock wheel model of a car (FORMAT-CARS, Wheels and Tests).
struct WheelCensus {
  bool parsed = false, kind3 = false, morph = false, lod0_pool = false, finite = false;
  float design_width = 0, base_radius = 0;
};

float be_float(const std::vector<uint8_t>& head, size_t at) {
  const uint32_t bits = uint32_t(head[at]) << 24 | uint32_t(head[at + 1]) << 16 | uint32_t(head[at + 2]) << 8 |
                        uint32_t(head[at + 3]);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

WheelCensus census_wheel(const cars::FolderRef& ref, const std::string& media) {
  WheelCensus w;
  cars::Folder folder;
  std::string error;
  std::vector<uint8_t> data;
  fh1::Carbin model;
  if (!folder.open(ref, &error) || !folder.read(cars::lower(media) + ".carbin", data, &error) ||
      !fh1::parse_carbin(data, model, &error))
    return w;
  w.parsed = true;
  w.kind3 = model.kind == 3;
  if (model.head.size() >= 48) {
    float b[4];
    for (size_t k = 0; k < 4; ++k) b[k] = be_float(model.head, 32 + 4 * k);
    // The design faces (bytes 36 and 44); some rims hold +/-FLT_MAX at 32
    // and 40 (no x extent) and are still counted.
    w.finite = std::isfinite(b[1]) && std::isfinite(b[3]) && std::fabs(b[1]) < 1e30f && std::fabs(b[3]) < 1e30f;
    if (w.finite) w.design_width = b[1] - b[3];
  }
  for (const auto& part : model.parts) {
    const size_t n = part.lod0_pool.vertex_count();
    if (n == 0) continue;
    w.lod0_pool = true;
    if (part.damage.size() == 2 && part.damage[1].count == n && part.damage[1].stride >= 8) w.morph = true;
    for (const auto& v : part.lod0_pool.vertices) {
      const auto p = fh1::carbin_position(part, v);
      w.base_radius = std::max(w.base_radius, std::sqrt(p[1] * p[1] + p[2] * p[2]));
    }
  }
  return w;
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
      // FORMAT-CARS counts the textures of every offer ZIP (its census
      // extracts the offer ZIPs). The resolver also mounts the loose
      // <offer>_pri_<n>/ folders beside them (audio, and the Rally and Car
      // Challenge packs' UI textures), which are not in that census.
      if (dlc_extracted.empty() && fs::is_directory(archive)) continue;
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
  // Stock wheels, wheel rows and colour rows (FORMAT-CARS, Tests): one car
  // per distinct media name (its lowest Id), over both columns.
  uint64_t kind3 = 0, morph = 0, no_morph = 0, no_lod0 = 0, morph14 = 0, finite = 0, width225 = 0;
  uint64_t off_x = 0, off_y = 0, off_z = 0, track_offsets = 0, first_stripe = 0, first_stripe_black = 0,
           atlas_rows = 0;
  std::set<int64_t> off_cars, atlas_cars;
  {
    std::set<std::string> done;
    for (const auto& r : db.cars) {
      for (const auto& col : r.colours) {
        const std::string t = cars::lower(col.base_texture);
        if (!t.empty() && t != "nodamage.tga") {
          ++atlas_rows;
          atlas_cars.insert(r.id);
        }
      }
      if (!r.colours.empty() && r.colours[0].has_stripe) {
        ++first_stripe;
        if (r.colours[0].stripe_rgb == 0) ++first_stripe_black;
      }
      if (!done.insert(cars::lower(r.media_name)).second) continue;
      if (r.has_track_offsets) ++track_offsets;
      const auto wheel_ref = r.wheel_media.empty() ? std::nullopt : install.find_wheel(r.wheel_media);
      if (wheel_ref) {
        const WheelCensus w = census_wheel(*wheel_ref, r.wheel_media);
        kind3 += w.kind3;
        if (!w.lod0_pool) ++no_lod0;
        else if (w.morph) ++morph;
        else ++no_morph;
        if (w.morph && w.base_radius >= 0.190f && w.base_radius <= 0.196f) ++morph14;
        if (w.finite) {
          ++finite;
          if (std::fabs(w.design_width - 0.225f) <= 0.001f) ++width225;
        }
      }
      // Rows 5 LF, 6 RF, 7 RR, 8 LR against the track rule (body frame).
      static const int kRows[4] = {5, 6, 7, 8};
      for (int k = 0; k < 4; ++k) {
        const int row = kRows[k];
        const size_t axle = row <= 6 ? 0 : 1;
        const double side = (row == 5 || row == 8) ? -1 : 1;
        const double width = double(r.tyre_width_mm[axle]) / 1000;
        const double outer = double(r.rim_inches[axle]) * 0.0254 / 2 + width * double(r.tyre_aspect[axle]) / 100;
        const double x = side * (r.track_outer[axle] - width) / 2;
        const auto& p = r.part_positions[size_t(row)];
        // y in the model's frame: the inverse pose of the body-frame height.
        const double lift = (r.ride_height[0] + r.ride_height[1]) / 2 - r.body_bottom_y;
        const double pitch = r.wheelbase > 0 ? std::atan((r.ride_height[1] - r.ride_height[0]) / r.wheelbase) : 0;
        const double y = std::cos(pitch) * (outer - lift) + std::sin(pitch) * p[2];
        const bool bx = std::fabs(p[0] - x) > 0.01, by = std::fabs(p[1] - y) > 0.01;
        off_x += bx;
        off_y += by;
        if (bx || by) off_cars.insert(r.id);
      }
    }
  }
  std::printf("[wheels] kind 3: %llu, LOD0 morph / without / no LOD0 pool: %llu / %llu / %llu, 14-inch morph: %llu, "
              "finite header: %llu (0.225 m: %llu)\n",
              (unsigned long long)kind3, (unsigned long long)morph, (unsigned long long)no_morph,
              (unsigned long long)no_lod0, (unsigned long long)morph14, (unsigned long long)finite,
              (unsigned long long)width225);
  std::printf("[wheel rows] off the track rule by > 1 cm in x / y / z: %llu / %llu / %llu on %zu cars; "
              "CarTrackOffsets cars %llu\n",
              (unsigned long long)off_x, (unsigned long long)off_y, (unsigned long long)off_z, off_cars.size(),
              (unsigned long long)track_offsets);
  std::printf("[colours] exceptions %zu rows, %zu cars, %zu stripe, %zu colour2; first rows with a stripe %llu "
              "(black %llu); rows with another atlas %llu on %zu cars\n",
              db.exception_rows, db.exception_cars, db.exception_stripes, db.exception_color2,
              (unsigned long long)first_stripe, (unsigned long long)first_stripe_black,
              (unsigned long long)atlas_rows, atlas_cars.size());
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
    expect("stock wheel models with header kind 3", kind3, 240);
    expect("stock rims with a LOD0 morph target", morph, 160);
    expect("stock rims without", no_morph, 76);
    expect("stock rims with no LOD0 rim pool", no_lod0, 4);
    expect("morph rims with a 14-inch design", morph14, 140);
    expect("rims with finite header bounds", finite, 239);
    expect("rims with a 0.225 m design width (within 1 mm)", width225, 82);
    expect("wheel rows more than 1 cm off the track rule in x", off_x, 321);
    expect("wheel rows more than 1 cm off in y", off_y, 249);
    expect("wheel rows more than 1 cm off in z", off_z, 0);
    expect("cars with wheel rows off the track rule", off_cars.size(), 133);
    expect("cars with CarTrackOffsets rows", track_offsets, 54);
    expect("Combo_Colors_Exceptions rows", db.exception_rows, 674);
    expect("Combo_Colors_Exceptions cars", db.exception_cars, 61);
    expect("Combo_Colors_Exceptions rows with HasStripe", db.exception_stripes, 661);
    expect("Combo_Colors_Exceptions rows with HasColor2", db.exception_color2, 0);
    expect("merged first colour rows with a stripe exception", first_stripe, 21);
    expect("of them with stripe colour 0", first_stripe_black, 2);
    expect("colour rows naming an atlas other than nodamage.tga", atlas_rows, 229);
    expect("cars with such rows", atlas_cars.size(), 30);
  }
  return failures ? 1 : 0;
}
