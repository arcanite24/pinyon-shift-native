#include "setup/on_device_baker.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "audio/bake.h"
#include "audio/sources.h"
#include "cars/bake.h"
#include "cars/bake_format.h"
#include "cars/database.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "cars/util.h"
#include "setup/xiso.h"
#include "world/world.h"

namespace setup::baker {
namespace {

namespace fs = std::filesystem;

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20u) out.push_back(c);
        break;
    }
  }
  return out;
}

bool is_non_empty_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0;
}

bool write_bytes_atomic(const fs::path& dest, const std::vector<uint8_t>& bytes, std::string* error) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  if (ec) {
    if (error) *error = "cannot create directory for " + dest.string() + ": " + ec.message();
    return false;
  }
  const fs::path tmp = dest.string() + ".tmp";
  if (!cars::bake::write_file(tmp, bytes, error)) {
    fs::remove(tmp, ec);
    return false;
  }
  fs::remove(dest, ec);
  ec.clear();
  fs::rename(tmp, dest, ec);
  if (ec) {
    fs::remove(tmp, ec);
    if (error) *error = "cannot finalize " + dest.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool write_text_atomic(const fs::path& dest, const std::string& text, std::string* error) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  const fs::path tmp = dest.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (error) *error = "cannot open " + tmp.string();
      return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
      if (error) *error = "write error on " + tmp.string();
      return false;
    }
  }
  fs::remove(dest, ec);
  ec.clear();
  fs::rename(tmp, dest, ec);
  if (ec) {
    fs::remove(tmp, ec);
    if (error) *error = "cannot rename " + dest.string() + ": " + ec.message();
    return false;
  }
  return true;
}

struct BakeJob {
  enum class Kind { kShared, kTyre, kWheel, kCar } kind = Kind::kCar;
  std::string key;
  cars::FolderRef ref;
  const cars::CarRow* row = nullptr;
  std::vector<const cars::CarRow*> rows;
  std::string own_tyre;
  fs::path rel_out;
};

struct JobOutcome {
  bool ok = false;
  bool reused = false;
  fs::path rel_out;
  uint64_t bytes = 0;
  std::string digest;
  std::string error;
};

void execute_job(const BakeJob& job, const fs::path& bake_dir, bool resume, JobOutcome& out) {
  out.rel_out = job.rel_out;
  const fs::path full_out = bake_dir / job.rel_out;
  std::error_code ec;
  if (resume && is_non_empty_file(full_out)) {
    out.ok = true;
    out.reused = true;
    out.bytes = fs::file_size(full_out, ec);
    return;
  }

  cars::bake::Bake b;
  std::string err;
  bool ok = false;
  switch (job.kind) {
    case BakeJob::Kind::kShared:
      ok = cars::bake_shared(job.ref, b, &err);
      break;
    case BakeJob::Kind::kTyre:
      ok = cars::bake_wheel(job.ref, true, b, &err);
      break;
    case BakeJob::Kind::kWheel:
      ok = cars::bake_wheel(job.ref, false, b, &err);
      break;
    case BakeJob::Kind::kCar:
      ok = cars::bake_car(job.ref, *job.row, b, &err);
      if (ok && !job.own_tyre.empty()) b.car.tyre_media = b.car.tyre_texture_media = job.own_tyre;
      if (ok) {
        std::vector<const cars::CarRow*> ordered = {job.row};
        for (const cars::CarRow* r : job.rows) {
          if (r != job.row) ordered.push_back(r);
        }
        for (const cars::CarRow* r : ordered) {
          b.car.stock_rows.push_back({r->id, r->stock_parts, r->removed_parts, r->race_livery, r->has_stock_body});
        }
      }
      break;
  }
  if (!ok) {
    out.error = job.key + ": " + err;
    return;
  }

  const auto bytes = cars::bake::encode(b);
  if (!write_bytes_atomic(full_out, bytes, &err)) {
    out.error = err;
    return;
  }
  cars::Sha256 h;
  h.update(bytes);
  out.digest = cars::Sha256::hex(h.finish());
  out.bytes = bytes.size();
  out.ok = true;
}

bool prepare_tuning_fallback(audio::SoundFiles& files, const std::string& media, const fs::path& sound_dir) {
  std::string text;
  std::string err;
  const std::string et_file = media + "_ET.xml";
  if (!files.read_tuning(audio::TuningKind::kEngine, et_file, text, &err)) return false;
  std::error_code ec;
  const fs::path car_dir = sound_dir / media;
  fs::create_directories(car_dir, ec);
  std::ofstream out(car_dir / et_file, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return bool(out);
}

}  // namespace

const std::vector<std::string>& fast_start_car_names() {
  static const std::vector<std::string> kStarter = {
      // Hero / showcase + campaign starter + early festival rivals & prizes
      "nis_skylinegtrvspecii_02",
      "vw_corradovr6_95",
      "for_mustangboss302_13",
      "srt_vipergts_13",
      "maz_rx7_97",
      "mit_lancerevoxgsr_08",
      "sub_imprezawrxsti_11",
      "nis_370z_10",
      "hon_s2000_03",
      "bmw_m3_08",
      "aud_r8gt_11",
      "fer_458italia_09",
      "lam_aventador_12",
      "for_focusrs_09",
      "dod_chargerrt_69",
      "che_camarozl1_12",
      "vw_golfr_10",
      "min_coopers_09",
      "toy_supra_98",
      "nis_silvia_00",
      // Common traffic cars
      "trf_fordfiesta_09",
      "trf_for_taurus_10",
      "trf_vw_jetta_03",
      "trf_bmw_323ti_00",
      "trf_che_express_06",
      "trf_dod_ram_06",
      "trf_gmc_vandura_83",
      "trf_sub_legacy_05",
      "trf_toy_prius_11",
      "trf_toy_rav4_06",
  };
  return kStarter;
}

unsigned recommended_bake_threads(unsigned hw_concurrency, bool is_mobile, unsigned requested) {
  const unsigned hw = hw_concurrency == 0 ? 4u : hw_concurrency;
  if (requested > 0) return std::clamp(requested, 1u, std::max(1u, hw));
  if (is_mobile) {
    // Cap on Android SoCs (e.g. Snapdragon 8 Gen 2 / Odin 2 Portal) to stay within
    // sustained thermal budget and memory headroom while keeping bake throughput high.
    return std::clamp(hw / 2u, 2u, 4u);
  }
  return std::clamp(hw, 1u, 8u);
}

std::vector<std::string> select_cars_for_mode(const std::vector<std::string>& available_cars, BakeMode mode,
                                              const std::vector<std::string>& custom_cars) {
  std::set<std::string> avail;
  for (const auto& c : available_cars) avail.insert(lower_ascii(c));

  std::vector<std::string> out;
  if (!custom_cars.empty()) {
    std::set<std::string> seen;
    for (const auto& c : custom_cars) {
      const std::string k = lower_ascii(c);
      if (avail.count(k) && seen.insert(k).second) out.push_back(k);
    }
    return out;
  }

  if (mode == BakeMode::kFullRoster) {
    out.assign(avail.begin(), avail.end());
    return out;
  }

  // Fast-Start mode: pick curated starters + up to 8 traffic vehicles present in DB.
  std::set<std::string> chosen;
  for (const auto& k : fast_start_car_names()) {
    if (avail.count(k) && chosen.insert(k).second) out.push_back(k);
  }
  size_t traffic_added = 0;
  for (const auto& k : avail) {
    if (traffic_added >= 8) break;
    if (k.rfind("trf_", 0) == 0 && chosen.insert(k).second) {
      out.push_back(k);
      ++traffic_added;
    }
  }
  // Fallback for minimal / custom discs that lack the standard starter names.
  if (out.empty()) {
    for (const auto& k : avail) {
      out.push_back(k);
      if (out.size() >= 16) break;
    }
  }
  return out;
}

bool write_play_txt(const fs::path& play_dir, const fs::path& game_root, std::string* error) {
  std::error_code ec;
  fs::create_directories(play_dir, ec);
  fs::path value = game_root;
  // Prefer relative "game" when the extracted game lives inside play_dir/game
  // so the play folder remains self-contained and relocatable.
  const fs::path canon_play = fs::weakly_canonical(play_dir, ec);
  const fs::path canon_game = fs::weakly_canonical(game_root, ec);
  if (!canon_play.empty() && !canon_game.empty() && canon_game == canon_play / "game") {
    value = fs::path("game");
  }
  return write_text_atomic(play_dir / "play.txt", "game " + value.generic_string() + "\n", error);
}

bool read_bake_state(const fs::path& bake_dir, BakeStateRecord& out) {
  out = BakeStateRecord{};
  std::ifstream in(bake_dir / "bake-state.txt");
  if (!in) return false;
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    const std::string key = line.substr(0, sp);
    const std::string val = line.substr(sp + 1);
    if (key == "mode") out.mode = val;
    else if (key == "baked_cars") out.baked_cars = static_cast<size_t>(std::strtoull(val.c_str(), nullptr, 10));
    else if (key == "total_available_cars") out.total_available_cars = static_cast<size_t>(std::strtoull(val.c_str(), nullptr, 10));
    else if (key == "has_shared") out.has_shared = (val == "1");
    else if (key == "has_world_index") out.has_world_index = (val == "1");
    else if (key == "complete") out.complete = (val == "1");
  }
  return true;
}

bool write_bake_state(const fs::path& bake_dir, const BakeStateRecord& state, std::string* error) {
  std::ostringstream ss;
  ss << "mode " << (state.mode.empty() ? "fast_start" : state.mode) << "\n"
     << "baked_cars " << state.baked_cars << "\n"
     << "total_available_cars " << state.total_available_cars << "\n"
     << "has_shared " << (state.has_shared ? 1 : 0) << "\n"
     << "has_world_index " << (state.has_world_index ? 1 : 0) << "\n"
     << "complete " << (state.complete ? 1 : 0) << "\n";
  return write_text_atomic(bake_dir / "bake-state.txt", ss.str(), error);
}

bool run_on_device_bake(const BakeOptions& opts, const ProgressCallback& progress, const LogCallback& log,
                        const std::atomic<bool>* cancel, BakeReport& report) {
  report = BakeReport{};
  const auto t0 = std::chrono::steady_clock::now();

  auto emit_log = [&](const std::string& msg) {
    if (log) log(msg);
  };
  auto emit_progress = [&](Stage st, size_t done, size_t total, float overall, const std::string& label,
                           const std::string& detail) {
    if (progress) {
      progress({st, static_cast<int>(st), 7, done, total, std::clamp(overall, 0.0f, 1.0f), label, detail});
    }
  };
  auto is_cancelled = [&]() { return cancel && cancel->load(std::memory_order_relaxed); };

  if (opts.game_root.empty() || opts.play_dir.empty()) {
    report.error = "game_root and play_dir are required";
    return false;
  }

  emit_progress(Stage::kInspect, 0, 1, 0.02f, "Inspecting game disc", opts.game_root.string());
  emit_log("Discovering game files under " + opts.game_root.string());

  cars::Install install;
  cars::Database db;
  std::string err;
  if (!cars::discover_install(opts.game_root, opts.state_root, {}, install, &err)) {
    report.error = "inspect failed: " + err;
    emit_log("ERROR: " + report.error);
    return false;
  }
  const bool db_loaded = cars::load_database(install, db, &err);
  if (!db_loaded) {
    if (opts.bake_cars) {
      report.error = "inspect failed: " + err;
      emit_log("ERROR: " + report.error);
      return false;
    }
    if (!is_non_empty_file(install.media / "db" / "gamedb.slt")) {
      report.error = "inspect failed: missing media/db/gamedb.slt";
      emit_log("ERROR: " + report.error);
      return false;
    }
    report.warnings.push_back("database inspect skipped in zero-bake mode: " + err);
    err.clear();
  }
  if (is_cancelled()) {
    report.cancelled = true;
    report.error = "bake cancelled";
    return false;
  }

  std::map<std::string, const cars::CarRow*> chosen_rows;
  for (const auto& r : db.cars) {
    const std::string key = cars::lower(r.media_name);
    auto& slot = chosen_rows[key];
    if (!slot || (r.selectable && !slot->selectable)) slot = &r;
  }

  std::vector<std::string> all_available;
  all_available.reserve(chosen_rows.size());
  for (const auto& [k, _] : chosen_rows) all_available.push_back(k);
  report.cars_total_available = db_loaded ? all_available.size() : install.base_cars.size();

  const std::vector<std::string> selected_cars =
      opts.bake_cars ? select_cars_for_mode(all_available, opts.mode, opts.custom_cars) : std::vector<std::string>{};
  if (opts.bake_cars && selected_cars.empty()) {
    report.error = "no cars matched in game database";
    return false;
  }

  const fs::path bake_dir = opts.play_dir / "bake";
  std::error_code ec;
  if (opts.bake_world_index) fs::create_directories(opts.play_dir / "world", ec);
  if (opts.bake_sounds) fs::create_directories(opts.play_dir / "sound", ec);

  if (opts.bake_cars) {
    fs::create_directories(bake_dir / "cars", ec);
    fs::create_directories(bake_dir / "wheels", ec);

    emit_log("Selected " + std::to_string(selected_cars.size()) + " of " + std::to_string(all_available.size()) +
             " cars (" + (opts.mode == BakeMode::kFullRoster ? "Full Roster" : "Fast Start") + ")");

    // Stage 2: Physics tables
    emit_progress(Stage::kPhysics, 0, selected_cars.size(), 0.08f, "Extracting car physics", "physics.zip");
    {
      std::vector<int64_t> ids;
      std::vector<std::string> keys;
      for (const auto& k : selected_cars) {
        ids.push_back(chosen_rows.at(k)->id);
        keys.push_back(k);
      }
      cars::PhysicsSettings phys_settings;
      std::vector<cars::CarPhysics> phys_rows;
      if (!cars::load_physics_settings(install.media / "physics.zip", phys_settings, &err) ||
          !cars::load_car_physics(install, ids, phys_rows, &err)) {
        report.error = "physics extraction failed: " + err;
        emit_log("ERROR: " + report.error);
        return false;
      }
      for (size_t i = 0; i < phys_rows.size(); ++i) {
        if (is_cancelled()) {
          report.cancelled = true;
          report.error = "bake cancelled";
          return false;
        }
        const fs::path pfile = bake_dir / "cars" / (keys[i] + ".physics.txt");
        if (opts.resume && is_non_empty_file(pfile)) continue;
        if (!write_text_atomic(pfile, cars::physics_to_text(phys_rows[i], phys_settings), &err)) {
          report.error = err;
          return false;
        }
      }
    }
    emit_progress(Stage::kPhysics, selected_cars.size(), selected_cars.size(), 0.14f, "Car physics ready",
                  std::to_string(selected_cars.size()) + " cars");

    // Build shared / wheel / tyre / car job queues
    std::vector<BakeJob> base_jobs;
    std::vector<BakeJob> car_jobs;
    std::set<std::string> wheel_seen;

    if (auto shared = install.base_cars.find("shared"); shared != install.base_cars.end()) {
      base_jobs.push_back({BakeJob::Kind::kShared, "shared", shared->second, nullptr, {}, {}, "shared.shiftbake"});
    } else {
      report.error = "missing media/cars/Shared.zip";
      return false;
    }
    for (const char* tyre : {cars::kTyreModel, cars::kTyreTextures}) {
      if (auto t = install.find_wheel(tyre)) {
        base_jobs.push_back(
            {BakeJob::Kind::kTyre, tyre, *t, nullptr, {}, {}, fs::path("wheels") / (std::string(tyre) + ".shiftbake")});
      } else {
        report.warnings.push_back(std::string("missing tyre archive ") + tyre);
      }
    }

    for (const std::string& key : selected_cars) {
      const cars::CarRow* row = chosen_rows.at(key);
      auto ref = install.find_car(row->media_name);
      if (!ref) {
        report.warnings.push_back("car archive missing for " + key);
        continue;
      }
      BakeJob cj;
      cj.kind = BakeJob::Kind::kCar;
      cj.key = key;
      cj.ref = *ref;
      cj.row = row;
      cj.rel_out = fs::path("cars") / (key + ".shiftbake");
      for (const auto& r : db.cars) {
        if (cars::lower(r.media_name) == key) cj.rows.push_back(&r);
      }
      const std::string own_tyre = std::string(cars::kTyreTextures) + key;
      if (auto t = install.find_wheel(own_tyre)) {
        cj.own_tyre = own_tyre;
        if (wheel_seen.insert(own_tyre).second) {
          base_jobs.push_back(
              {BakeJob::Kind::kTyre, own_tyre, *t, nullptr, {}, {}, fs::path("wheels") / (own_tyre + ".shiftbake")});
        }
      }
      car_jobs.push_back(std::move(cj));

      const std::string wheel = cars::lower(row->wheel_media);
      if (!wheel.empty() && wheel != cars::kTyreModel && wheel != cars::kTyreTextures && wheel_seen.insert(wheel).second) {
        if (auto w = install.find_wheel(wheel)) {
          base_jobs.push_back(
              {BakeJob::Kind::kWheel, wheel, *w, nullptr, {}, {}, fs::path("wheels") / (wheel + ".shiftbake")});
        } else {
          report.warnings.push_back(key + ": missing wheel " + wheel);
        }
      }
    }

#ifdef __ANDROID__
    constexpr bool kIsMobile = true;
#else
    constexpr bool kIsMobile = false;
#endif
    const unsigned workers = recommended_bake_threads(std::thread::hardware_concurrency(), kIsMobile, opts.max_threads);
    emit_log("Baking with " + std::to_string(workers) + " worker thread(s)");

    auto run_pool = [&](const std::vector<BakeJob>& jobs, Stage stage, float frac_lo, float frac_hi,
                        const std::string& label, std::vector<JobOutcome>& outcomes) -> bool {
      outcomes.assign(jobs.size(), {});
      if (jobs.empty()) return true;

      std::atomic<size_t> next{0};
      std::atomic<size_t> done{0};
      std::mutex cb_mu;

      auto worker_fn = [&]() {
        while (true) {
          if (is_cancelled()) break;
          const size_t idx = next.fetch_add(1, std::memory_order_relaxed);
          if (idx >= jobs.size()) break;
          execute_job(jobs[idx], bake_dir, opts.resume, outcomes[idx]);
          const size_t finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
          const float f =
              frac_lo + (frac_hi - frac_lo) * (static_cast<float>(finished) / static_cast<float>(jobs.size()));
          std::lock_guard<std::mutex> lk(cb_mu);
          emit_progress(stage, finished, jobs.size(), f, label, jobs[idx].key);
        }
      };

      const unsigned nthreads = std::min<unsigned>(workers, static_cast<unsigned>(jobs.size()));
      std::vector<std::thread> threads;
      threads.reserve(nthreads > 0 ? nthreads - 1 : 0);
      for (unsigned t = 1; t < nthreads; ++t) threads.emplace_back(worker_fn);
      worker_fn();
      for (auto& th : threads) th.join();

      return !is_cancelled();
    };

    // Stage 3: Shared assets, tyres, and wheels (0.14 -> 0.30)
    emit_progress(Stage::kSharedAndWheels, 0, base_jobs.size(), 0.14f, "Baking shared assets & wheels", "shared");
    std::vector<JobOutcome> base_outcomes;
    if (!run_pool(base_jobs, Stage::kSharedAndWheels, 0.14f, 0.30f, "Baking shared assets & wheels", base_outcomes)) {
      report.cancelled = true;
      report.error = "bake cancelled";
      return false;
    }
    for (size_t i = 0; i < base_outcomes.size(); ++i) {
      if (!base_outcomes[i].ok) {
        if (base_jobs[i].kind == BakeJob::Kind::kShared) {
          report.error = base_outcomes[i].error;
          emit_log("ERROR: " + report.error);
          return false;
        }
        report.warnings.push_back(base_outcomes[i].error);
      } else if (base_jobs[i].kind != BakeJob::Kind::kShared) {
        ++report.wheels_baked;
      }
    }

    // Stage 4: Car models (0.30 -> 0.78)
    emit_progress(Stage::kCars, 0, car_jobs.size(), 0.30f, "Baking cars", "");
    std::vector<JobOutcome> car_outcomes;
    if (!run_pool(car_jobs, Stage::kCars, 0.30f, 0.78f, "Baking cars", car_outcomes)) {
      report.cancelled = true;
      report.error = "bake cancelled";
      return false;
    }
    for (const auto& o : car_outcomes) {
      if (!o.ok) {
        report.warnings.push_back(o.error);
      } else if (o.reused) {
        ++report.cars_reused;
      } else {
        ++report.cars_baked;
      }
    }
    if (report.cars_baked + report.cars_reused == 0) {
      report.error = "no cars were baked successfully";
      return false;
    }
    emit_log("Cars ready: " + std::to_string(report.cars_baked) + " baked, " + std::to_string(report.cars_reused) +
             " reused");
  } else {
    emit_log("Zero-bake direct-from-disc mode: skipping on-device car bake (" +
             std::to_string(report.cars_total_available) + " cars stream directly from disc)");
  }

  // Stage 5: Colorado World Index (0.78 -> 0.90)
  if (opts.bake_world_index) {
    emit_progress(Stage::kWorldIndex, 0, 1, opts.bake_cars ? 0.80f : 0.25f, "Building Colorado world index",
                  "colorado.index");
    const fs::path colorado_dir = install.media / "tracks" / "colorado";
    const fs::path index_path = opts.play_dir / "world" / "colorado.index";
    if (fs::is_regular_file(colorado_dir / "bin.zip", ec)) {
      world::World w;
      if (!w.open(colorado_dir, index_path, &err)) {
        if (opts.require_world_index) {
          report.error = "world index failed: " + err;
          return false;
        }
        report.warnings.push_back("world index skipped: " + err);
      } else {
        report.world_index_ready = is_non_empty_file(index_path);
        emit_log("Colorado world index ready (" + std::to_string(w.models().size()) + " models)");
      }
    } else if (opts.require_world_index) {
      report.error = "missing media/tracks/colorado/bin.zip";
      return false;
    } else {
      report.warnings.push_back("colorado/bin.zip not found; world index skipped");
    }
    emit_progress(Stage::kWorldIndex, 1, 1, 0.90f, "Colorado world index ready", "colorado.index");
  }

  if (is_cancelled()) {
    report.cancelled = true;
    report.error = "bake cancelled";
    return false;
  }

  // Stage 6: Engine sound prep (0.90 -> 0.96)
  if (opts.bake_sounds && (!selected_cars.empty() || chosen_rows.count("nis_skylinegtrvspecii_02") > 0)) {
    emit_progress(Stage::kAudioPrep, 0, 1, 0.91f, "Preparing audio", "sound/");
    audio::SoundFiles sf;
    if (sf.open(install, &err)) {
      const fs::path ffmpeg = audio::find_ffmpeg();
      const std::string hero = chosen_rows.count("nis_skylinegtrvspecii_02") ? "NIS_SkylineGTRVSpecII_02"
                                                                              : chosen_rows.at(selected_cars[0])->media_name;
      if (!ffmpeg.empty()) {
        audio::BakeReport ar;
        if (audio::bake_engine_sound(sf, hero, opts.play_dir / "sound", ffmpeg, ar, &err)) {
          ++report.sounds_prepared;
          emit_log("Decoded engine audio for " + hero + " (" + std::to_string(ar.samples) + " samples)");
        } else {
          report.warnings.push_back("soundbake skipped: " + err);
        }
      } else {
        if (prepare_tuning_fallback(sf, hero, opts.play_dir / "sound")) {
          ++report.sounds_prepared;
        }
        emit_log("Prepared audio tuning folder (FFmpeg XMA decode optional on Android)");
      }
    } else {
      report.warnings.push_back("audio banks not present: " + err);
    }
    emit_progress(Stage::kAudioPrep, 1, 1, 0.96f, "Audio prepared", "sound/");
  }

  // Stage 7: Write play.txt & optional bake-state.txt (0.96 -> 1.00)
  emit_progress(Stage::kFinalize, 0, 1, 0.97f, "Finalizing play folder", "play.txt");
  if (!write_play_txt(opts.play_dir, opts.game_root, &err)) {
    report.error = "failed writing play.txt: " + err;
    return false;
  }
  report.play_txt_written = true;

  size_t total_baked_on_disk = 0;
  if (opts.bake_cars) {
    // Count total baked cars on disk (cumulative across Fast Start + Full Roster resumes).
    for (const auto& e : fs::directory_iterator(bake_dir / "cars", ec)) {
      if (e.is_regular_file(ec) && e.path().extension() == ".shiftbake") ++total_baked_on_disk;
    }

    BakeStateRecord st;
    st.mode = (opts.mode == BakeMode::kFullRoster) ? "full_roster" : "fast_start";
    st.baked_cars = total_baked_on_disk;
    st.total_available_cars = all_available.size();
    st.has_shared = is_non_empty_file(bake_dir / "shared.shiftbake");
    st.has_world_index = is_non_empty_file(opts.play_dir / "world" / "colorado.index");
    st.complete = (total_baked_on_disk >= all_available.size());
    write_bake_state(bake_dir, st, nullptr);
  }

  report.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  report.ok = true;
  if (opts.bake_cars) {
    emit_progress(Stage::kDone, 1, 1, 1.0f, "Setup complete",
                  std::to_string(total_baked_on_disk) + " car(s) ready");
    emit_log("On-device bake finished in " + std::to_string(static_cast<int>(report.elapsed_seconds)) + "s");
  } else {
    emit_progress(Stage::kDone, 1, 1, 1.0f, "Setup complete",
                  std::to_string(report.cars_total_available) + " car(s) ready (direct from disc)");
    emit_log("Zero-bake setup finished in " + std::to_string(static_cast<int>(report.elapsed_seconds)) + "s");
  }
  return true;
}

LibraryStatus inspect_library(const fs::path& play_dir, const fs::path& state_dir) {
  LibraryStatus st;
  std::error_code ec;

  // Locate game root from play.txt or default play_dir/game.
  fs::path candidate_game = play_dir / "game";
  if (fs::is_regular_file(play_dir / "play.txt", ec)) {
    st.has_play_txt = true;
    std::ifstream in(play_dir / "play.txt");
    for (std::string line; std::getline(in, line);) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("game ", 0) == 0) {
        fs::path g(line.substr(5));
        if (g.is_relative()) g = play_dir / g;
        candidate_game = g;
        break;
      }
    }
  }

  const auto disc = xiso::inspect_disc_directory(candidate_game);
  st.has_game_disc = disc.valid;
  st.game_root = disc.resolved_root.empty() ? candidate_game : disc.resolved_root;
  st.disc_car_count = disc.car_zip_count;

  const fs::path bake_dir = play_dir / "bake";
  st.has_shared_bake = is_non_empty_file(bake_dir / "shared.shiftbake");
  st.has_world_index = is_non_empty_file(play_dir / "world" / "colorado.index");
  st.has_sound_dir = fs::is_directory(play_dir / "sound", ec);

  if (fs::is_directory(bake_dir / "cars", ec)) {
    for (const auto& e : fs::directory_iterator(bake_dir / "cars", ec)) {
      if (e.is_regular_file(ec) && e.path().extension() == ".shiftbake") ++st.baked_car_count;
    }
  }

  BakeStateRecord rec;
  if (read_bake_state(bake_dir, rec)) {
    st.full_roster_complete = rec.complete && st.baked_car_count >= rec.total_available_cars && rec.total_available_cars > 0;
  }

  st.ready_to_play = st.has_play_txt && st.has_game_disc && st.has_world_index;

  if (!state_dir.empty()) {
    st.has_v4 = is_non_empty_file(state_dir / "title-update-v4" / "media.zip");
    st.packages = stfs::list_installed_packages(state_dir, play_dir);
    st.mods = mods::list_mods(state_dir / "mods");
    st.turnip_driver_path = mods::find_installed_turnip_driver(state_dir / "drivers");
    st.has_turnip_driver = !st.turnip_driver_path.empty();
  }

  fs::path space_probe = play_dir;
  while (!space_probe.empty() && !fs::exists(space_probe, ec)) space_probe = space_probe.parent_path();
  if (!space_probe.empty()) {
    const auto sp = fs::space(space_probe, ec);
    if (!ec) st.free_storage_bytes = sp.available;
  }

  if (st.ready_to_play) {
    if (st.baked_car_count > 0) {
      st.summary = "Ready to play (" + std::to_string(st.disc_car_count) + " cars on disc, " +
                   std::to_string(st.baked_car_count) + " baked)";
    } else {
      st.summary = "Ready to play (" + std::to_string(st.disc_car_count) + " cars direct from disc)";
    }
  } else if (st.has_game_disc) {
    st.summary = "Game disc extracted (" + std::to_string(st.disc_car_count) + " cars) — ready to index";
  } else {
    st.summary = "Select your Forza Horizon .iso or extracted folder to begin";
  }
  return st;
}

std::string library_status_to_json(const LibraryStatus& st) {
  std::ostringstream ss;
  ss << "{"
     << "\"has_game_disc\":" << (st.has_game_disc ? "true" : "false") << ","
     << "\"game_root\":\"" << json_escape(st.game_root.generic_string()) << "\","
     << "\"disc_car_count\":" << st.disc_car_count << ","
     << "\"has_play_txt\":" << (st.has_play_txt ? "true" : "false") << ","
     << "\"has_shared_bake\":" << (st.has_shared_bake ? "true" : "false") << ","
     << "\"has_world_index\":" << (st.has_world_index ? "true" : "false") << ","
     << "\"has_sound_dir\":" << (st.has_sound_dir ? "true" : "false") << ","
     << "\"baked_car_count\":" << st.baked_car_count << ","
     << "\"full_roster_complete\":" << (st.full_roster_complete ? "true" : "false") << ","
     << "\"ready_to_play\":" << (st.ready_to_play ? "true" : "false") << ","
     << "\"has_v4\":" << (st.has_v4 ? "true" : "false") << ","
     << "\"has_turnip_driver\":" << (st.has_turnip_driver ? "true" : "false") << ","
     << "\"turnip_driver_path\":\"" << json_escape(st.turnip_driver_path.generic_string()) << "\","
     << "\"free_storage_bytes\":" << st.free_storage_bytes << ","
     << "\"summary\":\"" << json_escape(st.summary) << "\","
     << "\"packages\":[";
  for (size_t i = 0; i < st.packages.size(); ++i) {
    const auto& p = st.packages[i];
    if (i > 0) ss << ",";
    ss << "{\"id\":\"" << json_escape(p.package_id) << "\","
       << "\"name\":\"" << json_escape(p.display_name) << "\","
       << "\"kind\":\"" << stfs::package_kind_name(p.kind) << "\","
       << "\"enabled\":" << (p.enabled ? "true" : "false") << ","
       << "\"puboffer\":" << p.puboffer << "}";
  }
  ss << "],\"mods\":[";
  for (size_t i = 0; i < st.mods.size(); ++i) {
    const auto& m = st.mods[i];
    if (i > 0) ss << ",";
    ss << "{\"name\":\"" << json_escape(m.name) << "\","
       << "\"title\":\"" << json_escape(m.title) << "\","
       << "\"version\":\"" << json_escape(m.version) << "\","
       << "\"summary\":\"" << json_escape(m.summary) << "\","
       << "\"enabled\":" << (m.enabled ? "true" : "false") << ","
       << "\"order\":" << m.order_index << "}";
  }
  ss << "]}";
  return ss.str();
}

}  // namespace setup::baker
