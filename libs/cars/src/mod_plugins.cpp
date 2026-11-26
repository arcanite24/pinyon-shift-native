#include "cars/mod_plugins.h"

#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cars::plugins {
namespace fs = std::filesystem;

struct Host::Loaded {
  Host* host = nullptr;
  size_t index = 0;  // into plugins_
  void* handle = nullptr;
  ShiftModTickFn tick = nullptr;
  ShiftModShutdownFn shutdown = nullptr;
  void* state = nullptr;
  std::string name, dir_utf8;
  std::map<std::string, std::string> options;
  ShiftModHost api{};
};

namespace {

void* open_library(const fs::path& p, std::string& error) {
#if defined(_WIN32)
  HMODULE h = LoadLibraryW(p.wstring().c_str());
  if (!h) error = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")";
  return reinterpret_cast<void*>(h);
#else
  void* h = dlopen(p.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    // dlerror() clears its message, so a second call returns null.
    const char* why = dlerror();
    error = why ? why : "dlopen failed";
  }
  return h;
#endif
}

void* symbol(void* handle, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

void close_library(void* handle) {
  if (!handle) return;
#if defined(_WIN32)
  FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

}  // namespace

const char* library_suffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

Host::Host() = default;
Host::~Host() { shutdown(); }

size_t Host::alive() const {
  return size_t(std::count_if(plugins_.begin(), plugins_.end(), [](const Plugin& p) { return p.alive; }));
}

bool Host::load(const std::string& mod, const fs::path& mod_dir, const fs::path& library,
                std::map<std::string, std::string> options, std::string* error) {
  Plugin rec;
  rec.mod = mod;
  rec.library = library;
  auto fail = [&](const std::string& why, void* handle) {
    close_library(handle);
    rec.error = why;
    plugins_.push_back(rec);
    if (error) *error = why;
    if (log_) log_(mod, "refused: " + why);
    return false;
  };
  std::string why;
  void* handle = open_library(library, why);
  if (!handle) return fail(why, nullptr);
  auto abi = reinterpret_cast<ShiftModAbiVersionFn>(symbol(handle, "shift_mod_abi_version"));
  auto init = reinterpret_cast<ShiftModInitFn>(symbol(handle, "shift_mod_init"));
  if (!abi || !init) return fail("missing shift_mod_abi_version or shift_mod_init", handle);
  const uint32_t version = abi();
  if (version != SHIFT_MOD_ABI_VERSION)
    return fail("built for ABI " + std::to_string(version) + ", the game has " + std::to_string(SHIFT_MOD_ABI_VERSION),
                handle);
  auto l = std::make_unique<Loaded>();
  l->host = this;
  l->index = plugins_.size();
  l->handle = handle;
  l->tick = reinterpret_cast<ShiftModTickFn>(symbol(handle, "shift_mod_tick"));
  l->shutdown = reinterpret_cast<ShiftModShutdownFn>(symbol(handle, "shift_mod_shutdown"));
  l->name = mod;
  const auto dir_u8 = mod_dir.generic_u8string();
  l->dir_utf8.assign(reinterpret_cast<const char*>(dir_u8.data()), dir_u8.size());
  l->options = std::move(options);
  ShiftModHost& api = l->api;
  api.size = sizeof(ShiftModHost);
  api.abi = SHIFT_MOD_ABI_VERSION;
  api.mod_name = l->name.c_str();
  api.mod_dir = l->dir_utf8.c_str();
  api.host_data = l.get();
  api.log = [](void* data, const char* message) {
    auto* self = static_cast<Loaded*>(data);
    if (self->host->log_) self->host->log_(self->name, message ? message : "");
  };
  api.option = [](void* data, const char* key) -> const char* {
    auto* self = static_cast<Loaded*>(data);
    auto it = key ? self->options.find(key) : self->options.end();
    return it == self->options.end() ? nullptr : it->second.c_str();
  };
  api.notify = [](void* data, const char* message, float seconds) {
    auto* self = static_cast<Loaded*>(data);
    if (self->host->notify_) self->host->notify_(self->name, message ? message : "", seconds);
  };
  const int rc = init(&l->api, &l->state);
  if (rc != 0) return fail("shift_mod_init returned " + std::to_string(rc), handle);
  rec.alive = true;
  plugins_.push_back(rec);
  loaded_.push_back(std::move(l));
  if (log_) log_(mod, "loaded " + library.filename().string());
  return true;
}

void Host::tick(const ShiftModFrame& frame) {
  for (auto& l : loaded_) {
    if (!l->handle || !plugins_[l->index].alive) continue;
    if (l->tick) l->tick(l->state, &frame);
    ++plugins_[l->index].ticks;
  }
}

void Host::shutdown() {
  for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
    auto& l = **it;
    if (!l.handle) continue;
    if (plugins_[l.index].alive && l.shutdown) l.shutdown(l.state);
    plugins_[l.index].alive = false;
    close_library(l.handle);
    l.handle = nullptr;
  }
  loaded_.clear();
}

}  // namespace cars::plugins
