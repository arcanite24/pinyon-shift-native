// Where the engine sound's files are: the tuning archives on the disc
// (media/audio/cars/CarModelTuning.zip, Engines/EngineTuning.zip,
// Engines/HarmonicTuning.zip), the same folders inside each package's offer
// ZIPs and v4's media.zip, and the loose FSB4 banks in
// Engines/Soundbanks/LOD1/ on the disc and in each package's loose folder.
// Members resolve v4 over the packages (later priority over earlier) over
// the disc; names match without case.
// Spec: SPEC-ENGINE-SOUND "Where a car's engine sound lives".
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/engine_sound.h"
#include "audio/fsb4.h"
#include "cars/sources.h"

namespace fh1 {
class Archive;
}

namespace audio {

enum class TuningKind { kEngine, kHarmonic, kModel };

class SoundFiles {
 public:
  SoundFiles();
  ~SoundFiles();
  bool open(const cars::Install& install, std::string* error);

  // A tuning file's text by its file name (e.g. "NIS_..._ET.xml").
  bool read_tuning(TuningKind kind, const std::string& file, std::string& text, std::string* error);
  // A bank's path on disk, or empty.
  std::filesystem::path bank(const std::string& name) const;

 private:
  struct Member {
    size_t archive;
    std::string name;
  };
  std::vector<std::unique_ptr<fh1::Archive>> archives_;
  std::map<std::string, Member> tunings_[3];  // lower-case file name
  std::map<std::string, std::filesystem::path> banks_;
};

// A car's engine sound at the given intake/exhaust upgrade levels (L0..L3;
// default stock L0), its loop indices resolved against the banks. `banks`
// receives each group's bank headers (by bank name).
struct LoadedSound {
  EngineSound sound;
  std::map<std::string, std::vector<FsbSample>> banks;  // lower-case bank name
  std::vector<std::string> notes;                       // missing files and silent groups
};
bool load_engine_sound(SoundFiles& files, const std::string& media, LoadedSound& out, std::string* error,
                       UpgradeLevels levels = {});

std::string lower_name(std::string s);

}  // namespace audio
