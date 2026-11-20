// The engine sound's bake: each loop sample decoded to PCM once (spec
// "Bake each sample once to PCM"), next to copies of the car's tuning
// files, so a drive needs neither the archives nor a decoder.
//
// <dir>/<media>/sound.txt   lines: "engine <file>", "model <file>",
//                           "harmonic <element> <file>",
//                           "sample <bank> <index> <channels> <rate> <frames>"
// <dir>/<media>/<file>      the tuning XMLs, as read
// <dir>/<media>/pcm/<bank>.<index>.c<channel>.s16   mono s16le, <frames> long
//
// XMA is decoded by FFmpeg's xma2 decoder through its command-line tool,
// run once per sample at bake time (nothing is linked).
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "audio/engine_player.h"
#include "audio/sources.h"

namespace audio {

struct BakeReport {
  int samples = 0;            // decoded
  int length_mismatches = 0;  // decoded length off the header's (fixed to it)
  double worst_seam = 0;      // largest step at a loop's wrap, full scale 1
  std::vector<std::string> notes;
};

bool bake_engine_sound(SoundFiles& files, const std::string& media, const std::filesystem::path& out_dir,
                       const std::filesystem::path& ffmpeg, BakeReport& report, std::string* error);

// A baked car: its sound (loop indices resolved) and each loop's PCM.
struct BakedSound {
  LoadedSound loaded;
  // (lower-case bank, sample, channel) -> PCM.
  std::map<std::tuple<std::string, int, int>, PcmSample> pcm;
  // The player's pcm table: per group, per loop.
  std::vector<std::vector<const PcmSample*>> table() const;
};
bool load_baked_sound(const std::filesystem::path& out_dir, const std::string& media, BakedSound& out,
                      std::string* error);

// FFmpeg on PATH, or in WinGet's package folder on Windows; empty if none.
std::filesystem::path find_ffmpeg();

}  // namespace audio
