// carbake: bakes every distinct car model of the base disc, v4 and the
// installed packages to our own format, with its textures, materials,
// wheels and tyres.
//
//   carbake --game-root <dir> --state-root <dir> --out <dir>
//           [--dlc-extracted <dir>] [--threads N] [--cars name,name,...]
//
// Writes <out>/cars/<media>.shiftbake (one per distinct media name, lower
// case), <out>/wheels/<wheel>.shiftbake (every stock wheel and the tyre
// archives), <out>/shared.shiftbake, <out>/manifest.txt (SHA-256 and size
// of every baked file, sorted), <out>/bake-report.txt (per car: source,
// row, notes, duplicate copies), and <out>/bake-time.txt (wall time; the
// only output that differs between two bakes). Reads the player's files
// read-only.
//
// Spec: FORMAT-CARS; AUTOSHOW_REFERENCE (The cars); plan row M1 (bake).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#include "cars/bake.h"
#include "cars/bake_format.h"
#include "cars/database.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "cars/util.h"

namespace {

namespace fs = std::filesystem;

struct Job {
  enum class Kind { kCar, kWheel, kTyre, kShared } kind;
  std::string key;  // lower-case media name
  cars::FolderRef ref;
  const cars::CarRow* row = nullptr;
  std::vector<cars::FolderRef> others;  // further copies of a car folder
  std::vector<const cars::CarRow*> rows;  // every Data_Car row of the model (stock parts)
  std::string tyre;  // the model's own tyre set TireA<MediaName> (lower case), when it has one
};

struct Result {
  bool ok = false;
  std::string error;
  fs::path file;
  uint64_t bytes = 0;
  std::string digest;
  std::vector<std::string> report;
};

std::string folder_digest(const cars::FolderRef& ref, std::string* error) {
  cars::Folder folder;
  if (!folder.open(ref, error)) return "";
  cars::Sha256 h;
  for (const std::string& name : folder.names()) {
    std::vector<uint8_t> data;
    if (!folder.read(name, data, error)) return "";
    h.update({reinterpret_cast<const uint8_t*>(name.data()), name.size()});
    h.update(data);
  }
  return cars::Sha256::hex(h.finish());
}

std::string describe(const cars::FolderRef& ref) {
  return ref.origin + ":" + ref.path.filename().string() + (ref.prefix.empty() ? "" : "!" + ref.prefix);
}

void run(const Job& job, const fs::path& out_dir, Result& r) {
  cars::bake::Bake b;
  std::string error;
  bool ok = false;
  switch (job.kind) {
    case Job::Kind::kCar:
      ok = cars::bake_car(job.ref, *job.row, b, &error);
      if (ok && !job.tyre.empty()) b.car.tyre_media = b.car.tyre_texture_media = job.tyre;
      // Each row's stock parts and factory-livery flag, the baked row first.
      if (ok) {
        std::vector<const cars::CarRow*> rows = {job.row};
        for (const cars::CarRow* r : job.rows)
          if (r != job.row) rows.push_back(r);
        for (const cars::CarRow* r : rows)
          b.car.stock_rows.push_back({r->id, r->stock_parts, r->removed_parts, r->race_livery, r->has_stock_body});
      }
      break;
    case Job::Kind::kWheel: ok = cars::bake_wheel(job.ref, false, b, &error); break;
    case Job::Kind::kTyre: ok = cars::bake_wheel(job.ref, true, b, &error); break;
    case Job::Kind::kShared: ok = cars::bake_shared(job.ref, b, &error); break;
  }
  if (!ok) {
    r.error = job.key + ": " + error;
    return;
  }
  const auto bytes = cars::bake::encode(b);
  r.file = job.kind == Job::Kind::kShared ? fs::path("shared.shiftbake")
           : job.kind == Job::Kind::kCar  ? fs::path("cars") / (job.key + ".shiftbake")
                                          : fs::path("wheels") / (job.key + ".shiftbake");
  if (!cars::bake::write_file(out_dir / r.file, bytes, &error)) {
    r.error = error;
    return;
  }
  cars::Sha256 h;
  h.update(bytes);
  r.digest = cars::Sha256::hex(h.finish());
  r.bytes = bytes.size();
  if (job.kind == Job::Kind::kCar) {
    const auto& c = b.car;
    size_t parts = 0, subs = 0, verts = 0;
    for (const auto& m : b.models)
      for (const auto& p : m.parts) {
        ++parts;
        subs += p.subsections.size();
        verts += p.lod_vertices.size() + p.lod0_vertices.size();
      }
    std::ostringstream s;
    s << job.key << ": row " << c.car_id << " \"" << c.display_name << "\", from " << describe(job.ref)
      << ", " << b.models.size() << " models, " << parts << " parts, " << subs << " sub-sections, " << verts
      << " vertices, " << b.textures.size() << " textures, wheel " << c.wheel_media << ", paint "
      << (c.colours.empty() ? std::string("none") : std::to_string(c.colours[0].rgb));
    r.report.push_back(s.str());
    for (const auto& n : c.notes) r.report.push_back("  note: " + n);
    if (!job.others.empty()) {
      const std::string mine = folder_digest(job.ref, &error);
      for (const auto& other : job.others) {
        const std::string theirs = folder_digest(other, &error);
        r.report.push_back(std::string("  copy: ") + describe(other) +
                           (theirs == mine ? " identical" : " DIFFERS from the copy used"));
      }
    }
  }
  r.ok = true;
}

}  // namespace

// --dump <file.shiftbake>: models, parts and sub-sections (names only).
int dump(const fs::path& path) {
  cars::bake::Bake b;
  std::string error;
  if (!cars::bake::read_file(path, b, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (b.kind == cars::bake::Kind::kCar) {
    std::printf("car %s (%lld) wheel %s, colours %zu\n", b.car.media_name.c_str(), (long long)b.car.car_id,
                b.car.wheel_media.c_str(), b.car.colours.size());
    std::printf("  pose lift %.4f pitch %.5f, track %.3f/%.3f, wheelbase %.3f\n", b.car.lift, b.car.pitch,
                b.car.track_outer[0], b.car.track_outer[1], b.car.wheelbase);
    for (size_t w = 0; w < 4; ++w)
      std::printf("  wheel centre %.3f %.3f %.3f (body %.3f %.3f %.3f)\n", b.car.wheel_centres[w][0],
                  b.car.wheel_centres[w][1], b.car.wheel_centres[w][2], b.car.wheel_centres_body[w][0],
                  b.car.wheel_centres_body[w][1], b.car.wheel_centres_body[w][2]);
    for (const auto& c : b.car.colours)
      if (c.has_stripe)
        std::printf("  colour %lld seq %lld stripe %06x\n", (long long)c.id, (long long)c.sequence, c.stripe_rgb);
    for (size_t a = 0; a < 2; ++a)
      std::printf("  %s tyre %lld/%lld R%lld, ride height %.3f\n", a == 0 ? "front" : "rear",
                  (long long)b.car.tyre_width_mm[a], (long long)b.car.tyre_aspect[a], (long long)b.car.rim_inches[a],
                  b.car.ride_height[a]);
  }
  for (const auto& m : b.models) {
    std::printf("model %s role %u corner %u kind %u", m.name.c_str(), unsigned(m.role), unsigned(m.corner),
                m.header_kind);
    if (m.header_kind == 3)
      std::printf(" bounds %g %g %g %g", m.header_bounds[0], m.header_bounds[1], m.header_bounds[2],
                  m.header_bounds[3]);
    std::printf("\n");
    for (const auto& p : m.parts) {
      std::printf("  part %-24s c(%.2f %.2f %.2f) e(%.2f %.2f %.2f) v %zu/%zu:", p.name.c_str(), p.bounds[0],
                  p.bounds[1], p.bounds[2], p.bounds[6], p.bounds[7], p.bounds[8], p.lod_vertices.size(),
                  p.lod0_vertices.size());
      for (const auto& s : p.subsections)
        if (s.lod <= 1) std::printf(" %s/%s%s", s.technique.c_str(), cars::bake::material_class_name(s.material),
                                    s.lod ? "@1" : "");
      std::printf("\n");
      if (m.role != cars::bake::Role::kWheel && m.role != cars::bake::Role::kTyre) continue;
      // Wheel and tyre models: each sub-section's axle (x) and radial extent.
      for (const auto& s : p.subsections) {
        if (s.lod > 1 || s.indices.empty()) continue;
        const auto& pool = s.lod == 0 ? p.lod0_vertices : p.lod_vertices;
        float x0 = 1e9f, x1 = -1e9f, r0 = 1e9f, r1 = 0;
        for (uint32_t i : s.indices) {
          if (i >= pool.size()) continue;
          const auto& v = pool[i].position;
          const float r = std::sqrt(v[1] * v[1] + v[2] * v[2]);
          x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]), r0 = std::min(r0, r), r1 = std::max(r1, r);
        }
        std::printf("    %-20s lod %d x %.3f..%.3f r %.3f..%.3f\n", s.technique.c_str(), s.lod, x0, x1, r0, r1);
        const auto& morphs = s.lod == 0 ? p.lod0_morphs : p.lod_morphs;
        for (size_t t = 0; t < morphs.size(); ++t) {
          float tx0 = 1e9f, tx1 = -1e9f, tr0 = 1e9f, tr1 = 0;
          for (uint32_t i : s.indices) {
            if (i >= morphs[t].size()) continue;
            const auto& v = morphs[t][i];
            const float r = std::sqrt(v[1] * v[1] + v[2] * v[2]);
            tx0 = std::min(tx0, v[0]), tx1 = std::max(tx1, v[0]), tr0 = std::min(tr0, r), tr1 = std::max(tr1, r);
          }
          std::printf("      morph %zu x %.3f..%.3f r %.3f..%.3f\n", t, tx0, tx1, tr0, tr1);
        }
      }
    }
  }
  for (const auto& t : b.textures)
    std::printf("texture %s format %u %ux%u levels %zu\n", t.name.c_str(), unsigned(t.format),
                t.levels.empty() ? 0 : t.levels[0].width, t.levels.empty() ? 0 : t.levels[0].height, t.levels.size());
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--dump") return dump(argv[2]);
  fs::path game, state, dlc_extracted, out_dir;
  unsigned threads = 0;
  std::set<std::string> only;
  bool physics_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game-root") game = next();
    else if (a == "--state-root") state = next();
    else if (a == "--dlc-extracted") dlc_extracted = next();
    else if (a == "--out") out_dir = next();
    else if (a == "--threads") threads = unsigned(std::stoul(next()));
    else if (a == "--physics-only") physics_only = true;
    else if (a == "--cars") {
      std::stringstream list(next());
      for (std::string n; std::getline(list, n, ',');) only.insert(cars::lower(n));
    } else {
      std::fprintf(stderr, "usage: carbake --game-root <dir> --state-root <dir> --out <dir> "
                           "[--dlc-extracted <dir>] [--threads N] [--cars a,b] [--physics-only]\n");
      return 2;
    }
  }
  if (game.empty() || out_dir.empty()) {
    std::fprintf(stderr, "carbake: --game-root and --out are required\n");
    return 2;
  }
  const auto start = std::chrono::steady_clock::now();
  cars::Install install;
  cars::Database db;
  std::string error;
  if (!cars::discover_install(game, state, dlc_extracted, install, &error) ||
      !cars::load_database(install, db, &error)) {
    std::fprintf(stderr, "carbake: %s\n", error.c_str());
    return 1;
  }

  // One row per distinct media name: a selectable row first, then the
  // lowest Id.
  std::map<std::string, const cars::CarRow*> chosen;
  for (const auto& r : db.cars) {
    const std::string key = cars::lower(r.media_name);
    auto& slot = chosen[key];
    if (!slot || (r.selectable && !slot->selectable)) slot = &r;
  }
  // Each car's physics rows and the game-wide settings, as text beside its
  // bake (SPEC-CAR-PHYSICS: the drive reads them; they are the player's own
  // data, like the bake).
  {
    std::vector<int64_t> ids;
    std::vector<std::string> keys;
    for (const auto& [key, row] : chosen) {
      if (!only.empty() && !only.count(key)) continue;
      ids.push_back(row->id);
      keys.push_back(key);
    }
    cars::PhysicsSettings settings;
    std::vector<cars::CarPhysics> rows;
    if (!cars::load_physics_settings(install.media / "physics.zip", settings, &error) ||
        !cars::load_car_physics(install, ids, rows, &error)) {
      std::fprintf(stderr, "carbake: physics: %s\n", error.c_str());
      return 1;
    }
    std::error_code ec;
    fs::create_directories(out_dir / "cars", ec);
    for (size_t i = 0; i < rows.size(); ++i)
      std::ofstream(out_dir / "cars" / (keys[i] + ".physics.txt"), std::ios::binary)
          << cars::physics_to_text(rows[i], settings);
    std::printf("carbake: physics rows for %zu cars\n", rows.size());
    if (physics_only) return 0;
  }
  std::vector<Job> jobs;
  std::vector<std::string> problems;
  std::set<std::string> wheels;
  if (auto shared = install.base_cars.find("shared"); shared != install.base_cars.end())
    jobs.push_back({Job::Kind::kShared, "shared", shared->second, nullptr, {}, {}});
  else
    problems.push_back("no cars/Shared.zip");
  for (const char* tyre : {cars::kTyreModel, cars::kTyreTextures}) {
    if (auto t = install.find_wheel(tyre)) jobs.push_back({Job::Kind::kTyre, tyre, *t, nullptr, {}, {}});
    else problems.push_back(std::string("no wheel archive ") + tyre);
  }
  size_t car_jobs = 0;
  for (const auto& [key, row] : chosen) {
    if (!only.empty() && !only.count(key)) continue;
    Job job{Job::Kind::kCar, key, {}, row, {}, {}};
    for (const auto& r : db.cars)
      if (cars::lower(r.media_name) == key) job.rows.push_back(&r);
    auto ref = install.find_car(row->media_name, &job.others);
    if (!ref) {
      problems.push_back(key + ": no car folder");
      continue;
    }
    job.ref = *ref;
    // FORMAT-CARS, Wheels: a model with its own tyre set (in its package or
    // on the disc) draws TireA<MediaName>; every other model the shared tyre.
    const std::string own_tyre = std::string(cars::kTyreTextures) + key;
    if (auto t = install.find_wheel(own_tyre)) {
      job.tyre = own_tyre;
      if (wheels.insert(own_tyre).second) jobs.push_back({Job::Kind::kTyre, own_tyre, *t, nullptr, {}, {}});
    }
    jobs.push_back(std::move(job));
    ++car_jobs;
    const std::string wheel = cars::lower(row->wheel_media);
    if (wheel.empty() || wheel == cars::kTyreModel || wheel == cars::kTyreTextures || !wheels.insert(wheel).second)
      continue;
    if (auto w = install.find_wheel(wheel)) jobs.push_back({Job::Kind::kWheel, wheel, *w, nullptr, {}});
    else problems.push_back(key + ": no wheel folder " + wheel);
  }
  // Big archives first keeps the threads busy to the end; the output does
  // not depend on the order (one file per job, results stored by index).
  std::vector<size_t> order(jobs.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::vector<uint64_t> size(jobs.size(), 0);
  for (size_t i = 0; i < jobs.size(); ++i) {
    std::error_code ec;
    size[i] = jobs[i].ref.kind == cars::FolderRef::Kind::kArchive ? fs::file_size(jobs[i].ref.path, ec) : 1u << 26;
  }
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return size[a] > size[b]; });
  std::vector<Result> results(jobs.size());
  std::printf("carbake: %zu cars, %zu wheel and tyre archives, shared; %zu Data_Car rows\n", car_jobs,
              wheels.size() + 2, db.cars.size());
  std::fflush(stdout);
  cars::parallel_for(order.size(), threads, [&](size_t i) { run(jobs[order[i]], out_dir, results[order[i]]); });

  // Reports in a fixed order: by output path.
  std::vector<size_t> by_path;
  for (size_t i = 0; i < jobs.size(); ++i) {
    if (!results[i].ok) problems.push_back(results[i].error);
    else by_path.push_back(i);
  }
  std::sort(by_path.begin(), by_path.end(),
            [&](size_t a, size_t b) { return results[a].file.generic_string() < results[b].file.generic_string(); });
  uint64_t total = 0;
  {
    std::ofstream manifest(out_dir / "manifest.txt", std::ios::binary);
    for (size_t i : by_path) {
      manifest << results[i].digest << "  " << results[i].bytes << "  " << results[i].file.generic_string() << "\n";
      total += results[i].bytes;
    }
  }
  {
    std::ofstream report(out_dir / "bake-report.txt", std::ios::binary);
    report << "carbake report (FORMAT-CARS). Rows: " << db.cars.size() << ", distinct media names: " << chosen.size()
           << ", cars baked: " << car_jobs << ", wheel and tyre archives: " << wheels.size() + 2 << "\n";
    report << "packages in order:";
    for (const auto& p : install.packages) report << " " << p.id << "(" << p.priority << ")";
    report << "\n";
    for (size_t i : by_path)
      for (const auto& line : results[i].report) report << line << "\n";
    for (const auto& p : problems) report << "problem: " << p << "\n";
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  {
    std::ofstream t(out_dir / "bake-time.txt", std::ios::binary);
    t << "cars " << car_jobs << ", files " << by_path.size() << ", bytes " << total << ", wall time " << seconds
      << " s, threads " << (threads ? threads : std::thread::hardware_concurrency()) << "\n";
  }
  for (const auto& p : problems) std::fprintf(stderr, "problem: %s\n", p.c_str());
  std::printf("carbake: %zu files, %.2f GB, %.1f s (%s)\n", by_path.size(), double(total) / 1e9, seconds,
              problems.empty() ? "ok" : "with problems");
  return problems.empty() ? 0 : 1;
}
