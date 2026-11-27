#include "fh1data/members.h"

#include "fh1data/fs_walk.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace fh1 {
namespace {

std::string base_lower(const std::string& name) {
  const auto slash = name.find_last_of("/\\");
  std::string out = slash == std::string::npos ? name : name.substr(slash + 1);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

bool MemberSource::open(const std::filesystem::path& path, std::string* error) {
  path_ = path;
  archive_.reset();
  shared_index_.reset();
  files_.clear();
  names_.clear();
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    // A walk that never throws and reads past MAX_PATH (fs_walk.h).
    walk_tree(path, [&](const std::filesystem::directory_entry& e) {
      std::error_code fec;
      if (e.is_regular_file(fec)) files_.emplace(base_lower(e.path().filename().string()), usable_path(e.path()));
      return true;
    });
    names_.reserve(files_.size());
    for (const auto& [name, _] : files_) names_.push_back(name);
    return true;
  }
  archive_ = Archive::open_shared(path, error);
  if (!archive_) return false;
  shared_index_ = archive_->base_name_index();
  return true;
}

bool MemberSource::contains(const std::string& name) const {
  const std::string key = base_lower(name);
  return archive_ ? (shared_index_ && shared_index_->by_name.count(key) != 0) : files_.count(key) != 0;
}

bool MemberSource::read(const std::string& name, std::vector<uint8_t>& out, std::string* error) {
  const std::string key = base_lower(name);
  if (archive_) {
    const auto it = shared_index_->by_name.find(key);
    if (it == shared_index_->by_name.end()) {
      if (error) *error = name + ": not in " + path_.string();
      return false;
    }
    auto r = archive_->read(*it->second);
    if (!r.ok()) {
      if (error) *error = name + ": " + r.error;
      return false;
    }
    out = std::move(r.data);
    return true;
  }
  const auto it = files_.find(key);
  if (it == files_.end()) {
    if (error) *error = name + ": not in " + path_.string();
    return false;
  }
  std::ifstream f(it->second, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot open " + it->second.string();
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

std::vector<std::string> MemberSource::with_suffix(const std::string& suffix) const {
  std::vector<std::string> out;
  for (const auto& n : names())
    if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
      out.push_back(n);
  return out;
}

}  // namespace fh1
