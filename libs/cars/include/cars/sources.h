// Where the car files are: the base disc's archives, the v4 title update,
// and the installed Marketplace packages' offer ZIPs.
//
// A car is one folder of members named after Data_Car.MediaName: on the
// disc a ZIP archive media/cars/<MediaName>.zip, in a package a folder
// media/cars/<MediaName>/ inside the package's <offer>_pri_<n>.zip. Wheels
// are media/wheels/<WheelMedia>.zip or a package's media/wheels/<WheelMedia>/.
// Media names compare without case. Folders are taken from the disc first,
// then from the packages in priority order.
// Spec: FORMAT-CARS (Where the files are: base cars, car members, v4, DLC
// packages, rule).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fh1data/archive.h"

namespace cars {

// A folder of members: a whole archive, a folder inside an archive, or a
// folder on disk. Member names are lower case, relative to the folder,
// with '/' separators (e.g. "liverymasks/masks.xml").
struct FolderRef {
  enum class Kind { kArchive, kArchiveFolder, kDirectory };
  Kind kind = Kind::kArchive;
  std::filesystem::path path;  // archive file or directory
  std::string prefix;          // kArchiveFolder: lower-case member prefix ending in '/'
  std::string origin;          // "base" or the package id
  std::string name;            // the folder's media name as spelled on disc
};

class Folder {
 public:
  bool open(const FolderRef& ref, std::string* error);
  const FolderRef& ref() const { return ref_; }
  // Sorted lower-case relative names.
  const std::vector<std::string>& names() const { return names_; }
  bool contains(const std::string& name) const;
  // Reads a member by its relative name (any case). Not thread-safe.
  bool read(const std::string& name, std::vector<uint8_t>& out, std::string* error);

 private:
  FolderRef ref_;
  std::unique_ptr<fh1::Archive> archive_;
  std::map<std::string, const fh1::ArchiveMember*> members_;
  std::map<std::string, std::filesystem::path> files_;
  std::vector<std::string> names_;
};

struct Package {
  std::string id;                       // folder name under the Marketplace root
  uint32_t priority = 0;                // lowest _pri_ number of its content archives
  std::vector<std::filesystem::path> archives;  // content archives, by priority
  // Lower-case media name -> folder (cars and wheels).
  std::map<std::string, FolderRef> cars, wheels;
  // Merge databases: (archive, member name), in archive and name order.
  std::vector<std::pair<std::filesystem::path, std::string>> merges;
  // SPEC-V4-CONTENT, Ownership: the publication (offer) number, from the
  // package's Media/<n>.puboffer (-1: none), and its licence mask, from the
  // state root's dlc/<package>.json (interim FFFFFFFF without the record).
  int64_t puboffer = -1;
  uint32_t licence_mask = 0xFFFFFFFFu;
};

struct Install {
  std::filesystem::path media;         // the disc's media folder
  std::filesystem::path v4_media_zip;  // <state root>/title-update-v4/media.zip (may be empty)
  std::map<std::string, FolderRef> base_cars, base_wheels;  // lower-case media name
  std::vector<Package> packages;       // ascending (priority, id)

  // Finds a car or wheel folder: the disc first, then packages in order.
  // `others` receives every further copy (for the duplicate report).
  std::optional<FolderRef> find_car(const std::string& media, std::vector<FolderRef>* others = nullptr) const;
  std::optional<FolderRef> find_wheel(const std::string& media, std::vector<FolderRef>* others = nullptr) const;
};

// The community package that is not official content; left out.
inline constexpr const char* kCommunityPackage = "4D5309C900000729";

// `game_root`: a folder holding the disc's media/ (or media itself).
// `state_root`: the launcher's state root (title-update-v4/ and
// user/0000000000000000/4D5309C9/00000002/<package>/Media/DLCZips/); may be
// empty for the disc alone. `dlc_extracted`: optionally a folder of
// extracted offer ZIPs laid out as <package>/<offer zip stem>/media/...,
// used instead of the installed ZIPs.
bool discover_install(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                      const std::filesystem::path& dlc_extracted, Install& out, std::string* error);

std::string lower(std::string s);

// One content package of the state root (SPEC-V4-CONTENT): its record
// under dlc/, and whether it is installed (folder under 00000002, header
// under Headers/00000002) and enabled (not under Disabled/). The community
// package is left out.
struct PackInfo {
  std::string id, name;  // name: the record's display_name (id without it)
  bool installed = false, header = false, enabled = false;
  bool owned() const { return installed && header && enabled; }
};
std::vector<PackInfo> list_packs(const std::filesystem::path& state_root);

}  // namespace cars
