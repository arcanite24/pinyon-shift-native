#include "profile/profiles.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace profile::profiles {
namespace fs = std::filesystem;

bool valid_gamertag(std::string_view tag, std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (tag.empty()) return fail("a gamertag needs at least one character");
  if (tag.size() > 15) return fail("a gamertag has at most 15 characters");
  if (!std::isalpha(static_cast<unsigned char>(tag[0]))) return fail("a gamertag starts with a letter");
  for (size_t i = 0; i < tag.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(tag[i]);
    if (c == ' ') {
      if (i + 1 == tag.size() || tag[i + 1] == ' ') return fail("single spaces between words only");
      continue;
    }
    if (!std::isalnum(c)) return fail("letters, digits and spaces only");
  }
  return true;
}

bool valid_profile_name(std::string_view name) {
  if (name.empty() || name.size() > 24) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

std::string name_from_gamertag(std::string_view tag) {
  std::string out;
  for (char c : tag) {
    if (std::isalnum(static_cast<unsigned char>(c))) out += char(std::tolower(static_cast<unsigned char>(c)));
    else if (c == ' ' && !out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.size() > 24) out.resize(24);
  return out.empty() ? "player" : out;
}

Manager::Manager(fs::path user_dir, fs::path default_save_dir)
    : user_dir_(std::move(user_dir)), default_dir_(std::move(default_save_dir)) {
  profiles_.push_back(Profile{"default", "Player", default_dir_});
}

bool Manager::load(std::string* error) {
  profiles_.clear();
  profiles_.push_back(Profile{"default", "Player", default_dir_});
  active_ = "default";
  std::ifstream in(file());
  if (!in) return true;  // a first run: only the default profile
  std::string line;
  if (!std::getline(in, line) || line.rfind("shift-profiles ", 0) != 0) {
    if (error) *error = file().string() + ": not a profiles file";
    return false;
  }
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("active ", 0) == 0) {
      active_ = line.substr(7);
    } else if (line.rfind("profile ", 0) == 0) {
      std::istringstream ls(line.substr(8));
      std::string name, tag;
      ls >> name;
      std::getline(ls, tag);
      while (!tag.empty() && tag.front() == ' ') tag.erase(tag.begin());
      if (!valid_profile_name(name)) continue;
      if (name == "default") {
        if (valid_gamertag(tag)) profiles_[0].gamertag = tag;
        continue;
      }
      if (find(name)) continue;
      profiles_.push_back(Profile{name, valid_gamertag(tag) ? tag : name, user_dir_ / "profiles" / name});
    }
  }
  if (!find(active_)) active_ = "default";
  return true;
}

bool Manager::save(std::string* error) const {
  std::ostringstream s;
  s << "shift-profiles 1\nactive " << active_ << "\n";
  for (const auto& p : profiles_) s << "profile " << p.name << " " << p.gamertag << "\n";
  std::error_code ec;
  fs::create_directories(user_dir_, ec);
  fs::path tmp = file();
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !(f << s.str())) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, file(), ec);
  if (ec && error) *error = "cannot replace " + file().string();
  return !ec;
}

const Profile& Manager::active() const {
  const Profile* p = find(active_);
  return p ? *p : profiles_.front();
}

const Profile* Manager::find(std::string_view name) const {
  for (const auto& p : profiles_)
    if (p.name == name) return &p;
  return nullptr;
}

bool Manager::create(std::string_view name, std::string_view gamertag, std::string* error) {
  if (!valid_profile_name(name)) {
    if (error) *error = "profile names are 1-24 of a-z, 0-9, - and _";
    return false;
  }
  if (find(name)) {
    if (error) *error = "there is already a profile " + std::string(name);
    return false;
  }
  if (!valid_gamertag(gamertag, error)) return false;
  Profile p{std::string(name), std::string(gamertag), user_dir_ / "profiles" / std::string(name)};
  std::error_code ec;
  fs::create_directories(p.save_dir, ec);
  if (ec) {
    if (error) *error = "cannot make " + p.save_dir.string();
    return false;
  }
  profiles_.push_back(std::move(p));
  return save(error);
}

bool Manager::set_gamertag(std::string_view name, std::string_view gamertag, std::string* error) {
  if (!valid_gamertag(gamertag, error)) return false;
  for (auto& p : profiles_)
    if (p.name == name) {
      p.gamertag = std::string(gamertag);
      return save(error);
    }
  if (error) *error = "no profile " + std::string(name);
  return false;
}

bool Manager::switch_to(std::string_view name, std::string* error) {
  if (!find(name)) {
    if (error) *error = "no profile " + std::string(name);
    return false;
  }
  active_ = std::string(name);
  return save(error);
}

bool Manager::remove(std::string_view name, std::string* error) {
  if (name == "default" || name == active_) {
    if (error) *error = "the default and the active profile cannot be removed";
    return false;
  }
  auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const Profile& p) { return p.name == name; });
  if (it == profiles_.end()) {
    if (error) *error = "no profile " + std::string(name);
    return false;
  }
  std::error_code ec;
  if (fs::exists(it->save_dir, ec)) {
    fs::path dst;
    for (int n = 1;; ++n) {
      dst = user_dir_ / "profiles" / (".deleted-" + it->name + "-" + std::to_string(n));
      if (!fs::exists(dst, ec)) break;
    }
    fs::rename(it->save_dir, dst, ec);
    if (ec) {
      if (error) *error = "cannot move " + it->save_dir.string() + " aside";
      return false;
    }
  }
  profiles_.erase(it);
  return save(error);
}

}  // namespace profile::profiles
