// Save profiles and gamertags (ours, Slice 13).
//
// The player can keep several named careers. The first profile, "default",
// is the user-data folder's drive/ (where the save has always been); every
// other profile is <user data>/profiles/<name>/ with its own save.txt and
// garage.txt. <user data>/profiles.txt records them:
//
//   shift-profiles 1
//   active default
//   profile default Player
//   profile weekend Weekend Racer
//
// (`profile <name> <gamertag>`; the gamertag may hold spaces). A gamertag is
// 1 to 15 characters: letters, digits and single spaces, starting with a
// letter, as on Xbox 360. Profile names are 1 to 24 lower-case letters,
// digits, '-' or '_'.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace profile::profiles {

struct Profile {
  std::string name, gamertag;
  std::filesystem::path save_dir;
};

bool valid_gamertag(std::string_view tag, std::string* why = nullptr);
bool valid_profile_name(std::string_view name);
// A profile name from a gamertag ("Weekend Racer" -> "weekend-racer").
std::string name_from_gamertag(std::string_view tag);

class Manager {
 public:
  // `user_dir`: the user-data folder; `default_save_dir`: the default
  // profile's folder (normally <user_dir>/drive).
  Manager(std::filesystem::path user_dir, std::filesystem::path default_save_dir);
  bool load(std::string* error = nullptr);
  bool save(std::string* error = nullptr) const;

  const std::vector<Profile>& profiles() const { return profiles_; }
  const Profile& active() const;
  const Profile* find(std::string_view name) const;
  bool create(std::string_view name, std::string_view gamertag, std::string* error);
  bool set_gamertag(std::string_view name, std::string_view gamertag, std::string* error);
  // Makes `name` the active profile (takes effect when the game next reads
  // the save, i.e. at the next start or a profile switch reload).
  bool switch_to(std::string_view name, std::string* error);
  // Deletes a profile that is neither the default nor the active one; its
  // folder moves to <user_dir>/profiles/.deleted-<name>-<n> (never erased).
  bool remove(std::string_view name, std::string* error);
  std::filesystem::path file() const { return user_dir_ / "profiles.txt"; }

 private:
  std::filesystem::path user_dir_, default_dir_;
  std::vector<Profile> profiles_;
  std::string active_ = "default";
};

}  // namespace profile::profiles
