// soundbake: decodes each car's engine loops to PCM once (SPEC-ENGINE-SOUND),
// next to copies of its tuning files, for carview --drive to play.
//
//   soundbake --game-root <dir> [--state-root <dir>] --out <dir>
//             (--cars a,b | --all) [--ffmpeg <exe>]
//
// XMA is decoded by FFmpeg's command-line tool (found on PATH or in WinGet's
// packages unless --ffmpeg names it), once per sample, at bake time only.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "audio/bake.h"
#include "audio/xma_decode.h"
#include "cars/database.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  fs::path game, state, out_dir, ffmpeg;
  std::vector<std::string> cars_wanted;
  bool all = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game-root") game = next();
    else if (a == "--state-root") state = next();
    else if (a == "--out") out_dir = next();
    else if (a == "--ffmpeg") ffmpeg = next();
    else if (a == "--all") all = true;
    else if (a == "--cars") {
      std::stringstream list(next());
      for (std::string n; std::getline(list, n, ',');)
        if (!n.empty()) cars_wanted.push_back(n);
    } else {
      std::fprintf(stderr, "usage: soundbake --game-root <dir> [--state-root <dir>] --out <dir> "
                           "(--cars a,b | --all) [--ffmpeg <exe>]\n");
      return 2;
    }
  }
  if (game.empty() || out_dir.empty() || (cars_wanted.empty() && !all)) {
    std::fprintf(stderr, "soundbake: --game-root, --out and --cars or --all are required\n");
    return 2;
  }
  if (ffmpeg.empty() && !audio::has_inprocess_xma2_decoder()) ffmpeg = audio::find_ffmpeg();
  if (ffmpeg.empty() && !audio::has_inprocess_xma2_decoder()) {
    std::fprintf(stderr, "soundbake: no ffmpeg found (install it, or pass --ffmpeg)\n");
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  cars::Install install;
  std::string error;
  if (!cars::discover_install(game, state, {}, install, &error)) {
    std::fprintf(stderr, "soundbake: %s\n", error.c_str());
    return 1;
  }
  if (all) {
    cars::Database db;
    if (!cars::load_database(install, db, &error)) {
      std::fprintf(stderr, "soundbake: %s\n", error.c_str());
      return 1;
    }
    std::set<std::string> seen;
    for (const auto& c : db.cars)
      if (seen.insert(cars::lower(c.media_name)).second) cars_wanted.push_back(c.media_name);
  }
  audio::SoundFiles files;
  if (!files.open(install, &error)) {
    std::fprintf(stderr, "soundbake: %s\n", error.c_str());
    return 1;
  }
  std::printf("ffmpeg: %s\n", ffmpeg.string().c_str());
  int baked = 0, failed = 0, samples = 0, mismatches = 0;
  double worst_seam = 0;
  for (const auto& media : cars_wanted) {
    audio::BakeReport report;
    if (!audio::bake_engine_sound(files, media, out_dir, ffmpeg, report, &error)) {
      std::printf("%s: FAILED %s\n", media.c_str(), error.c_str());
      ++failed;
      continue;
    }
    ++baked;
    samples += report.samples;
    mismatches += report.length_mismatches;
    worst_seam = std::max(worst_seam, report.worst_seam);
    std::printf("%s: %d samples, %d decoded to another length (fixed), worst seam step %.3f\n", media.c_str(),
                report.samples, report.length_mismatches, report.worst_seam);
    for (const auto& n : report.notes) std::printf("  note: %s\n", n.c_str());
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("soundbake: %d cars baked, %d failed; %d samples, %d length fixes, worst seam %.3f; %.1f s\n", baked,
              failed, samples, mismatches, worst_seam, secs);
  return failed && !baked ? 1 : 0;
}
