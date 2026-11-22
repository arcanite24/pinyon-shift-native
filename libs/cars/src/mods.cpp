#include "cars/mods.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace cars::mods {
namespace {

struct Layer {
  std::filesystem::path base;               // the play folder's bake or sound folder
  std::vector<std::filesystem::path> over;  // the mods' matching folders, the winner first
};
std::vector<Layer> g_layers;
std::vector<std::string> g_active;

// `p` relative to `base`, or empty when outside it.
std::filesystem::path relative_to(const std::filesystem::path& p, const std::filesystem::path& base) {
  if (base.empty()) return {};
  const auto rel = p.lexically_normal().lexically_relative(base.lexically_normal());
  if (rel.empty() || rel == "." || *rel.begin() == "..") return {};
  return rel;
}

}  // namespace

std::vector<Mod> list(const std::filesystem::path& root) {
  std::vector<Mod> out;
  std::error_code ec;
  if (root.empty() || !std::filesystem::is_directory(root, ec)) return out;
  for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
    if (!e.is_directory(ec)) continue;
    Mod m;
    m.name = e.path().filename().string();
    if (m.name.empty() || m.name[0] == '_' || m.name[0] == '.') continue;
    m.dir = e.path();
    m.title = m.name;
    std::ifstream in(m.dir / "mod.txt");
    if (!in) continue;
    for (std::string line; std::getline(in, line);) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("title ", 0) == 0) m.title = line.substr(6);
      else if (line.rfind("enabled ", 0) == 0) m.enabled = line.substr(8) != "0";
      else if (line.rfind("priority ", 0) == 0) m.priority = std::atoi(line.c_str() + 9);
    }
    for (const char* sub : {"bake", "sound", "media", "radio"}) {
      const auto d = m.dir / sub;
      if (!std::filesystem::is_directory(d, ec)) continue;
      for (const auto& f : std::filesystem::recursive_directory_iterator(d, ec))
        if (f.is_regular_file(ec)) ++m.files;
    }
    out.push_back(std::move(m));
  }
  std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) {
    return a.priority != b.priority ? a.priority > b.priority : a.name < b.name;
  });
  return out;
}

void activate(const std::vector<Mod>& mods, const std::filesystem::path& bake_dir,
              const std::filesystem::path& sound_dir, const std::filesystem::path& game_root) {
  g_layers.clear();
  g_active.clear();
  Layer bake{bake_dir, {}}, sound{sound_dir, {}};
  Layer media{game_root.empty() ? std::filesystem::path{} : game_root / "media", {}};
  Layer radio{game_root.empty() ? std::filesystem::path{} : game_root / "media" / "audio" / "radio", {}};
  std::error_code ec;
  for (const Mod& m : mods) {
    if (!m.enabled) continue;
    bool any = false;
    if (std::filesystem::is_directory(m.dir / "bake", ec)) bake.over.push_back(m.dir / "bake"), any = true;
    if (std::filesystem::is_directory(m.dir / "sound", ec)) sound.over.push_back(m.dir / "sound"), any = true;
    if (!media.base.empty() && std::filesystem::is_directory(m.dir / "media", ec))
      media.over.push_back(m.dir / "media"), any = true;
    // Pinyon Shift's layout for whole game files (the XE mod's too).
    if (!media.base.empty() && std::filesystem::is_directory(m.dir / "game" / "media", ec))
      media.over.push_back(m.dir / "game" / "media"), any = true;
    if (!radio.base.empty() && std::filesystem::is_directory(m.dir / "radio", ec))
      radio.over.push_back(m.dir / "radio"), any = true;
    if (any) g_active.push_back(m.name);
  }
  if (!bake.over.empty()) g_layers.push_back(std::move(bake));
  if (!sound.over.empty()) g_layers.push_back(std::move(sound));
  // Check more specific radio/ before media/ so either mod layout works.
  if (!radio.over.empty()) g_layers.push_back(std::move(radio));
  if (!media.over.empty()) g_layers.push_back(std::move(media));
}

std::filesystem::path resolve(const std::filesystem::path& p) {
  std::error_code ec;
  for (const Layer& l : g_layers) {
    const auto rel = relative_to(p, l.base);
    if (rel.empty()) continue;
    for (const auto& o : l.over) {
      const auto q = o / rel;
      if (std::filesystem::exists(q, ec)) return q;
    }
  }
  return p;
}

const std::vector<std::string>& active() { return g_active; }

}  // namespace cars::mods
