// The UI files the game mounts under media/ui, with the title update's
// copies first: v4's media.zip overlays the disc's media tree file by file
// (SPEC-V4-CONTENT, Package layout and mounts; FORMAT-UISCENES). A scene is
// three files resolved one by one, so a scene v4 ships only part of keeps
// the disc's other files (v4 ships 40 scene names: 36 whole, and four with
// one or two files).
//
//   disc media/UI.zip            Scenes/ui4/<name>.<fbf|bsg|bgf>
//   v4   media.zip               ui/scenes/ui4/<name>.<ext>        (lower case)
//   disc media/ui/Textures.zip   <rel>.xds          (texture path "T:<rel>")
//   v4   media.zip               ui/textures/<rel>.xds
//   disc media/ui/textures/Horizon.zip   <rel>.xds  (texture path "<rel>")
//   v4   media.zip               ui/textures/horizon/<rel>.xds
//   disc media/ui/<file>.xml  /  v4 media.zip ui/<file>.xml
//
// No DLC package ships UI scenes. The Rally and 1000 Club packages ship
// loose Media/UI/Textures; carview's texture resolver probes only the
// Rally package's (by CoDriverIconSet.xds) after the overlay misses.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/archive.h"
#include "fh1data/ui_scene.h"

namespace fh1 {

class UiOverlay {
 public:
  enum class Origin { kNone, kDisc, kV4 };
  struct File {
    ReadResult read;
    Origin origin = Origin::kNone;
    bool ok() const { return origin != Origin::kNone && read.ok(); }
  };

  // `game_root`: the folder holding the disc's media/. `v4_media_zip`: the
  // state root's title-update-v4/media.zip, or empty for the disc alone.
  UiOverlay(std::filesystem::path game_root, std::filesystem::path v4_media_zip);
  // <state_root>/title-update-v4/media.zip when it exists, else empty.
  static std::filesystem::path v4_media_zip_of(const std::filesystem::path& state_root);

  bool has_v4();
  // One file of a scene; `ext` is "fbf", "bsg" or "bgf".
  File scene_file(std::string_view scene, std::string_view ext);
  // A scene: its .fbf and .bsg parsed, its .bgf when present. `fbf_origin`
  // and `bsg_origin` receive where each came from.
  bool load_scene(std::string_view scene, UiScene& out, std::string* error, Origin* fbf_origin = nullptr,
                  Origin* bsg_origin = nullptr);
  // A UI texture by its scene path ("T:<rel>" or "<rel>", no extension).
  File texture(std::string_view path);
  // A loose UI file under media/ui ("LoadingDefs.xml").
  File ui_file(std::string_view name);
  // The scene names whose .fbf v4 ships (lower case, sorted).
  std::vector<std::string> v4_scene_names();

 private:
  Archive* disc(std::unique_ptr<Archive>& slot, bool& tried, const std::filesystem::path& path);
  Archive* v4();
  std::filesystem::path game_root_, v4_path_;
  std::unique_ptr<Archive> v4_, ui_, textures_, horizon_;
  bool v4_tried_ = false, ui_tried_ = false, textures_tried_ = false, horizon_tried_ = false;
};

}  // namespace fh1
