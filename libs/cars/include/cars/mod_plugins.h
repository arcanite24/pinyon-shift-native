// The native mod plugin loader (ours, Slice 13): loads shift_mod_api.h
// libraries, checks their ABI and runs init / tick / shutdown.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cars/shift_mod_api.h"

namespace cars::plugins {

struct Plugin {
  std::string mod;
  std::filesystem::path library;
  bool alive = false;  // initialised and not shut down
  std::string error;   // why it was refused or failed
  uint64_t ticks = 0;
};

class Host {
 public:
  Host();
  ~Host();
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  using LogFn = std::function<void(const std::string& mod, std::string_view message)>;
  using NotifyFn = std::function<void(const std::string& mod, std::string_view message, float seconds)>;
  void set_log(LogFn fn) { log_ = std::move(fn); }
  void set_notify(NotifyFn fn) { notify_ = std::move(fn); }

  // Loads and initialises `library` for `mod`; false with `error` (the
  // plugin is still listed, not alive). `options`: the mod's choices.
  bool load(const std::string& mod, const std::filesystem::path& mod_dir, const std::filesystem::path& library,
            std::map<std::string, std::string> options, std::string* error);
  void tick(const ShiftModFrame& frame);
  // Shuts every live plugin down (in reverse load order) and unloads them.
  void shutdown();
  const std::vector<Plugin>& plugins() const { return plugins_; }
  size_t alive() const;

 private:
  struct Loaded;
  std::vector<std::unique_ptr<Loaded>> loaded_;
  std::vector<Plugin> plugins_;
  LogFn log_;
  NotifyFn notify_;
};

// The platform's shared library suffix (".dll", ".so", ".dylib").
const char* library_suffix();

}  // namespace cars::plugins
