// Members of one archive, read either from the archive itself or from a
// folder it was extracted to. Members are named by their lower-case base
// name (the first member wins when a name repeats, as the copies of a name
// in bin.zip are identical).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fh1data/archive.h"

namespace fh1 {

class MemberSource {
 public:
  // A folder (searched recursively) or an archive.
  bool open(const std::filesystem::path& path, std::string* error);

  // Lower-case base names, sorted.
  const std::vector<std::string>& names() const { return names_; }
  bool contains(const std::string& name) const;
  // Reads a member by name (any case). Not thread-safe for archives.
  bool read(const std::string& name, std::vector<uint8_t>& out, std::string* error);
  // The names ending in `suffix` (lower case).
  std::vector<std::string> with_suffix(const std::string& suffix) const;

  const std::filesystem::path& path() const { return path_; }
  bool is_archive() const { return archive_ != nullptr; }

 private:
  std::filesystem::path path_;
  std::unique_ptr<Archive> archive_;
  std::map<std::string, std::filesystem::path> files_;
  std::map<std::string, const ArchiveMember*> members_;
  std::vector<std::string> names_;
};

}  // namespace fh1
