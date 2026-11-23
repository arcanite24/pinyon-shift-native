// In-process on-device baker for standalone Android (and desktop) setup.
// Reuses libs/cars, libs/world, and libs/audio directly on thermal/core-capped
// background threads with live stage/item progress, resumability, fast-start
// vs full-roster modes, and play.txt generation.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "setup/mod_installer.h"
#include "setup/stfs.h"

namespace setup::baker {

enum class BakeMode {
  kFastStart,   // shared + world index + starter/campaign/traffic cars (fast playable start)
  kFullRoster,  // every distinct car in base disc + v4 + installed DLC packs
};

enum class Stage {
  kInspect = 0,
  kPhysics = 1,
  kSharedAndWheels = 2,
  kCars = 3,
  kWorldIndex = 4,
  kAudioPrep = 5,
  kFinalize = 6,
  kDone = 7,
};

struct BakeOptions {
  std::filesystem::path game_root;   // holds media/ (e.g. files/play/game)
  std::filesystem::path state_root;  // optional v4 + DLC state root (e.g. files/state)
  std::filesystem::path play_dir;    // output play folder (e.g. files/play)
  BakeMode mode = BakeMode::kFastStart;
  std::vector<std::string> custom_cars;  // optional explicit car media names (overrides mode)
  unsigned max_threads = 0;              // 0 = auto thermal/core cap
  bool resume = true;                    // skip already-baked files
  bool bake_world_index = true;
  bool bake_sounds = true;
  bool require_world_index = false;  // false = allow minimal/test trees without colorado/bin.zip
};

struct BakeProgress {
  Stage stage = Stage::kInspect;
  int stage_index = 0;
  int stage_count = 7;
  size_t item_done = 0;
  size_t item_total = 0;
  float overall_fraction = 0.0f;  // 0.0 .. 1.0
  std::string stage_label;
  std::string detail;
};

using ProgressCallback = std::function<void(const BakeProgress&)>;
using LogCallback = std::function<void(const std::string&)>;

struct BakeStateRecord {
  std::string mode;  // "fast_start" or "full_roster"
  size_t baked_cars = 0;
  size_t total_available_cars = 0;
  bool has_shared = false;
  bool has_world_index = false;
  bool complete = false;
};

struct BakeReport {
  bool ok = false;
  bool cancelled = false;
  size_t cars_baked = 0;
  size_t cars_reused = 0;
  size_t cars_total_available = 0;
  size_t wheels_baked = 0;
  size_t sounds_prepared = 0;
  bool world_index_ready = false;
  bool play_txt_written = false;
  double elapsed_seconds = 0.0;
  std::vector<std::string> warnings;
  std::string error;
};

struct LibraryStatus {
  bool has_game_disc = false;
  std::filesystem::path game_root;
  size_t disc_car_count = 0;
  bool has_play_txt = false;
  bool has_shared_bake = false;
  bool has_world_index = false;
  bool has_sound_dir = false;
  size_t baked_car_count = 0;
  bool full_roster_complete = false;
  bool ready_to_play = false;
  bool has_v4 = false;
  std::vector<stfs::InstalledPackage> packages;
  std::vector<mods::InstalledMod> mods;
  bool has_turnip_driver = false;
  std::filesystem::path turnip_driver_path;
  uint64_t free_storage_bytes = 0;
  std::string summary;
};

// Curated starter, campaign, hero, and traffic car media names for Fast-Start mode.
const std::vector<std::string>& fast_start_car_names();

// Thermal/core-capped worker thread count for on-device baking.
unsigned recommended_bake_threads(unsigned hw_concurrency, bool is_mobile, unsigned requested = 0);

// Selects lower-case car media names from all available DB cars for the given mode.
std::vector<std::string> select_cars_for_mode(const std::vector<std::string>& available_cars, BakeMode mode,
                                              const std::vector<std::string>& custom_cars = {});

bool write_play_txt(const std::filesystem::path& play_dir, const std::filesystem::path& game_root,
                    std::string* error = nullptr);
bool read_bake_state(const std::filesystem::path& bake_dir, BakeStateRecord& out);
bool write_bake_state(const std::filesystem::path& bake_dir, const BakeStateRecord& state,
                      std::string* error = nullptr);

bool run_on_device_bake(const BakeOptions& opts, const ProgressCallback& progress, const LogCallback& log,
                        const std::atomic<bool>* cancel, BakeReport& report);

LibraryStatus inspect_library(const std::filesystem::path& play_dir, const std::filesystem::path& state_dir);
std::string library_status_to_json(const LibraryStatus& st);

}  // namespace setup::baker
