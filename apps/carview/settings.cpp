#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdlib>

#include "platform_support.h"

namespace carview {
namespace {

std::filesystem::path portable_folder() {
  if (const char* base = SDL_GetBasePath()) {
#if defined(__APPLE__)
    constexpr bool kIsMac = true;
#else
    constexpr bool kIsMac = false;
#endif
    return resolve_portable_data_root(std::filesystem::path(base), kIsMac);
  }
  return {};
}

std::filesystem::path user_folder() {
  if (auto portable = portable_folder(); !portable.empty()) return portable;
  std::filesystem::path out;
  if (char* pref = SDL_GetPrefPath("shift-engine", "drive")) {
    out = std::filesystem::path(pref).parent_path().parent_path();  // ".../shift-engine/drive/" -> ".../shift-engine"
    SDL_free(pref);
  }
  return out;
}

}  // namespace

std::filesystem::path settings_path() {
  if (const char* e = std::getenv("SHIFT_SETTINGS"); e && *e) return e;
  const auto u = user_folder();
  return u.empty() ? u : u / "settings.txt";
}

std::filesystem::path default_user_dir() { return user_folder(); }

std::filesystem::path portable_user_dir() { return portable_folder(); }

std::filesystem::path default_mods_root() {
  const auto u = user_folder();
  return u.empty() ? u : u / "mods";
}

std::filesystem::path default_photos_dir() {
  const auto u = user_folder();
  return u.empty() ? std::filesystem::path("photos") : u / "photos";
}

std::filesystem::path default_replays_dir() {
  const auto u = user_folder();
  return u.empty() ? std::filesystem::path("replays") : u / "replays";
}

std::filesystem::path default_achievements_path() {
  const auto u = user_folder();
  return u.empty() ? std::filesystem::path("achievements.txt") : u / "achievements.txt";
}

}  // namespace carview
