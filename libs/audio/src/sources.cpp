#include "audio/sources.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

#include "cars/mods.h"
#include "fh1data/archive.h"

namespace audio {

std::string lower_name(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

namespace {

std::string base_name(const std::string& name) {
  const size_t slash = name.find_last_of("/\\");
  return slash == std::string::npos ? name : name.substr(slash + 1);
}

std::string slashes(std::string s) {
  for (char& c : s)
    if (c == '\\') c = '/';
  return lower_name(s);
}

// The folder under audio/cars/ that holds each kind, lower case.
const char* kFolders[3] = {"engines/enginetuning/", "engines/harmonictuning/", "carmodeltuning/"};
const char* kDiscArchives[3] = {"Engines/EngineTuning.zip", "Engines/HarmonicTuning.zip", "CarModelTuning.zip"};

// Finds a child of `dir` by name without case.
std::filesystem::path child_nocase(const std::filesystem::path& dir, const std::string& name) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return {};
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (lower_name(e.path().filename().string()) == lower_name(name)) return e.path();
  return {};
}

void add_banks(const std::filesystem::path& lod1, std::map<std::string, std::filesystem::path>& banks) {
  std::error_code ec;
  if (lod1.empty() || !std::filesystem::is_directory(lod1, ec)) return;
  for (const auto& e : std::filesystem::directory_iterator(lod1, ec))
    if (e.is_regular_file() && lower_name(e.path().extension().string()) == ".fsb")
      banks[lower_name(e.path().filename().string())] = e.path();
}

std::filesystem::path lod1_under(std::filesystem::path media) {
  for (const char* part : {"audio", "cars", "Engines", "Soundbanks", "LOD1"}) {
    media = child_nocase(media, part);
    if (media.empty()) return {};
  }
  return media;
}

}  // namespace

SoundFiles::SoundFiles() = default;
SoundFiles::~SoundFiles() = default;

bool SoundFiles::open(const cars::Install& install, std::string* error) {
  archives_.clear();
  for (auto& t : tunings_) t.clear();
  banks_.clear();
  auto add_archive = [&](const std::filesystem::path& path, bool whole_kind, int kind) -> bool {
    std::string e;
    auto a = fh1::Archive::open(path, &e);
    if (!a) {
      if (error) *error = path.string() + ": " + e;
      return false;
    }
    const size_t index = archives_.size();
    for (const auto& m : a->members()) {
      const std::string name = slashes(m.name);
      if (whole_kind) {
        tunings_[kind][lower_name(base_name(name))] = {index, m.name};
        continue;
      }
      // Package and v4 members: ".../audio/cars/<folder>/<file>".
      for (int k = 0; k < 3; ++k) {
        const std::string folder = std::string("audio/cars/") + kFolders[k];
        const size_t at = name.find(folder);
        if (at != std::string::npos && name.size() > at + folder.size() &&
            name.find('/', at + folder.size()) == std::string::npos)
          tunings_[k][base_name(name)] = {index, m.name};
      }
    }
    archives_.push_back(std::move(a));
    return true;
  };
  // The disc.
  const std::filesystem::path cars_dir = install.media / "audio" / "cars";
  for (int k = 0; k < 3; ++k) {
    const std::filesystem::path p = cars_dir / kDiscArchives[k];
    std::error_code ec;
    if (std::filesystem::exists(p, ec) && !add_archive(p, true, k)) return false;
  }
  add_banks(lod1_under(install.media), banks_);
  // The packages, in ascending priority (later wins), then v4.
  for (const auto& pkg : install.packages) {
    if (pkg.id == cars::kCommunityPackage) continue;
    for (const auto& zip : pkg.archives) {
      if (!add_archive(zip, false, 0)) return false;
      // Loose banks: <offer>_pri_<n>/Media/audio/cars/Engines/Soundbanks/LOD1/.
      std::filesystem::path loose = zip;
      loose.replace_extension();
      add_banks(lod1_under(child_nocase(loose, "Media")), banks_);
    }
  }
  if (!install.v4_media_zip.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(install.v4_media_zip, ec) && !add_archive(install.v4_media_zip, false, 0))
      return false;
  }
  return true;
}

bool SoundFiles::read_tuning(TuningKind kind, const std::string& file, std::string& text, std::string* error) {
  const auto& map = tunings_[int(kind)];
  const auto it = map.find(lower_name(base_name(file)));
  if (it == map.end()) {
    if (error) *error = "no tuning " + file;
    return false;
  }
  auto r = archives_[it->second.archive]->read(it->second.name);
  if (!r.ok()) {
    if (error) *error = file + ": " + r.error;
    return false;
  }
  text.assign(r.data.begin(), r.data.end());
  return true;
}

std::filesystem::path SoundFiles::bank(const std::string& name) const {
  const auto it = banks_.find(lower_name(base_name(name)));
  return it == banks_.end() ? std::filesystem::path{} : cars::mods::resolve(it->second);
}

bool load_engine_sound(SoundFiles& files, const std::string& media, LoadedSound& out, std::string* error,
                       UpgradeLevels levels) {
  out = {};
  out.sound.media = media;
  std::string text, e;
  XmlNode engine;
  if (!files.read_tuning(TuningKind::kEngine, media + "_ET.xml", text, &e) || !parse_xml(text, engine, &e)) {
    if (error) *error = media + ": engine tuning: " + e;
    return false;
  }
  std::vector<HarmonicRef> harmonics;
  if (!read_engine_tuning(engine, out.sound, harmonics, &e, levels)) {
    if (error) *error = media + ": " + e;
    return false;
  }
  for (const auto& h : harmonics) {
    XmlNode harmonic;
    if (!files.read_tuning(TuningKind::kHarmonic, h.file, text, &e) || !parse_xml(text, harmonic, &e)) {
      out.notes.push_back(h.element + ": " + e);
      continue;
    }
    if (!read_harmonic(engine, harmonic, h.element, out.sound, &e)) out.notes.push_back(h.element + ": " + e);
  }
  // Resolve each group's bank and loop indices; a group without its bank
  // is dropped (silent).
  std::vector<Group> kept;
  for (Group& g : out.sound.groups) {
    const std::string key = lower_name(g.bank);
    if (!out.banks.count(key)) {
      const auto path = files.bank(g.bank);
      std::vector<FsbSample> samples;
      std::ifstream in(path, std::ios::binary);
      std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (path.empty() || data.empty() || !read_fsb4(data, samples, &e)) {
        out.notes.push_back(g.name + ": bank " + g.bank + (path.empty() ? " not found" : ": " + e));
        continue;
      }
      out.banks[key] = std::move(samples);
    }
    const auto& samples = out.banks[key];
    std::vector<int> channels;
    for (const auto& s : samples) channels.push_back(s.channels);
    resolve_loop_indices(g.loops, int(samples.size()), channels);
    kept.push_back(std::move(g));
  }
  out.sound.groups = std::move(kept);
  XmlNode model;
  if (files.read_tuning(TuningKind::kModel, media + "_CMT.xml", text, &e) && parse_xml(text, model, &e))
    read_model_tuning(model, out.sound);
  else
    out.notes.push_back("no model tuning: every view level 1");
  return true;
}

}  // namespace audio
