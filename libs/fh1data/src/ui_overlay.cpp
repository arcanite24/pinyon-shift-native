#include "fh1data/ui_overlay.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>

namespace fh1 {
namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

}  // namespace

UiOverlay::UiOverlay(std::filesystem::path game_root, std::filesystem::path v4_media_zip)
    : game_root_(std::move(game_root)), v4_path_(std::move(v4_media_zip)) {}

std::filesystem::path UiOverlay::v4_media_zip_of(const std::filesystem::path& state_root) {
  if (state_root.empty()) return {};
  std::error_code ec;
  const auto p = state_root / "title-update-v4" / "media.zip";
  return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path{};
}

Archive* UiOverlay::disc(std::unique_ptr<Archive>& slot, bool& tried, const std::filesystem::path& path) {
  if (!tried) {
    tried = true;
    std::string e;
    if (!game_root_.empty()) slot = Archive::open(path, &e);
  }
  return slot.get();
}

Archive* UiOverlay::v4() {
  if (!v4_tried_) {
    v4_tried_ = true;
    std::string e;
    if (!v4_path_.empty()) v4_ = Archive::open(v4_path_, &e);
  }
  return v4_.get();
}

bool UiOverlay::has_v4() { return v4() != nullptr; }

UiOverlay::File UiOverlay::scene_file(std::string_view scene, std::string_view ext) {
  File f;
  const std::string tail = std::string(scene) + "." + std::string(ext);
  if (Archive* a = v4(); a && a->find("ui/scenes/ui4/" + tail)) {
    f.read = a->read("ui/scenes/ui4/" + tail);
    f.origin = Origin::kV4;
    return f;
  }
  if (Archive* a = disc(ui_, ui_tried_, game_root_ / "media" / "UI.zip"); a && a->find("Scenes/ui4/" + tail)) {
    f.read = a->read("Scenes/ui4/" + tail);
    f.origin = Origin::kDisc;
  }
  return f;
}

bool UiOverlay::load_scene(std::string_view scene, UiScene& out, std::string* error, Origin* fbf_origin,
                           Origin* bsg_origin) {
  const File fbf = scene_file(scene, "fbf");
  const File bsg = scene_file(scene, "bsg");
  if (fbf_origin) *fbf_origin = fbf.origin;
  if (bsg_origin) *bsg_origin = bsg.origin;
  if (!fbf.ok() || !bsg.ok()) {
    if (error) *error = !fbf.ok() ? (fbf.read.error.empty() ? "no .fbf" : fbf.read.error)
                                  : (bsg.read.error.empty() ? "no .bsg" : bsg.read.error);
    return false;
  }
  out = UiScene{};
  if (!parse_ui_fbf(fbf.read.data, out, error) || !parse_ui_bsg(bsg.read.data, out, error)) return false;
  const File bgf = scene_file(scene, "bgf");
  if (bgf.ok()) out.has_bgf = parse_ui_bgf(bgf.read.data, out.bgf, nullptr);
  return true;
}

UiOverlay::File UiOverlay::texture(std::string_view path) {
  File f;
  const bool t = path.rfind("T:", 0) == 0;
  const std::string rel = std::string(t ? path.substr(2) : path) + ".xds";
  const std::string v4_name = (t ? "ui/textures/" : "ui/textures/horizon/") + rel;
  if (Archive* a = v4(); a && a->find(v4_name)) {
    f.read = a->read(v4_name);
    f.origin = Origin::kV4;
    return f;
  }
  Archive* a = t ? disc(textures_, textures_tried_, game_root_ / "media" / "ui" / "Textures.zip")
                 : disc(horizon_, horizon_tried_, game_root_ / "media" / "ui" / "textures" / "Horizon.zip");
  if (a && a->find(rel)) {
    f.read = a->read(rel);
    f.origin = Origin::kDisc;
  }
  return f;
}

UiOverlay::File UiOverlay::ui_file(std::string_view name) {
  File f;
  const std::string v4_name = "ui/" + lower(name);
  if (Archive* a = v4(); a && a->find(v4_name)) {
    f.read = a->read(v4_name);
    f.origin = Origin::kV4;
    return f;
  }
  std::error_code ec;
  const auto p = game_root_ / "media" / "ui" / std::string(name);
  if (!game_root_.empty() && std::filesystem::is_regular_file(p, ec)) {
    std::ifstream in(p, std::ios::binary);
    f.read.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    f.origin = Origin::kDisc;
  }
  return f;
}

std::vector<std::string> UiOverlay::v4_scene_names() {
  std::set<std::string> names;
  if (Archive* a = v4()) {
    const std::string prefix = "ui/scenes/ui4/";
    for (const auto& m : a->members()) {
      const std::string n = normalize_member_name(m.name);
      if (n.size() > prefix.size() + 4 && n.rfind(prefix, 0) == 0 && n.compare(n.size() - 4, 4, ".fbf") == 0)
        names.insert(n.substr(prefix.size(), n.size() - prefix.size() - 4));
    }
  }
  return {names.begin(), names.end()};
}

}  // namespace fh1
