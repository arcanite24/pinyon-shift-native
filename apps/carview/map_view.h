// carview --drive: the minimal map (SPEC-FREEROAM-COLLECTIBLES, The minimal
// map), drawn on the CPU into opaque overlay panels like the HUD.
//
// - The minimap, lower left: the roads from colorado.nav by road_type, the
//   rumour circles and the icons, no background, turning with the car
//   (heading up) and tilted 45 degrees, about 300 m ahead of the car to
//   the far edge (the spec's interim zoom and tilt).
// - The pause map (M): north up over the background texture's low level,
//   the roads drawn over it, icons lying flat and the player's arrow turned
//   with the heading; zoom and pan are ours.
//
// The road and icon styles come from the game's map render profiles
// (MapProfileMinimap.xml, MapProfileFullscreen.xml): each road_type's
// under-line and line, each icon's atlas cell, size and colour. Profile
// sizes are the game's units, scaled here by our own factors.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "freeroam/collectibles.h"
#include "hud.h"

namespace carview {

struct MapMarker {
  enum Kind { kEvent, kStreetHub, kFestivalSite };
  Kind kind = kEvent;
  std::string icon_tag;  // MapProfile key: race, exhibition, streetrace, nemesisrace, racecentral, autoshow, workshop, paintshop
  std::string id;        // HorizonEventID, hub id, or site name
  std::string title;
  std::string subtitle;
  double x = 0, z = 0;
  bool is_new = false;
  bool completed = false;
  int site_or_hub = 0;  // FestivalSite::Kind or StreetHub::id
};

struct MapScene {
  double x = 0, z = 0;  // the player, game frame
  double heading = 0;   // rad, 0 faces +z (north), positive turns towards +x (east)
  const freeroam::Collectibles* data = nullptr;
  const freeroam::Progress* progress = nullptr;
  // Unfound signs and barns' exact places on the map (the game's Treasure
  // Map, or its collectibles cheat); found ones and rumours always show.
  bool reveal = false;
  // A race's opponents (x, z), drawn as dots (ours: the profiles' opponent
  // icon is not mapped yet).
  std::vector<std::array<double, 2>> opponents;
  // A race's route ahead (x, z), drawn green on the minimap (SPEC-UI-LAYOUTS a).
  std::vector<std::array<double, 2>> route;
  // Unlocked career events, Street Hubs, and Festival sites.
  std::vector<MapMarker> markers;
  int selected_marker = -1;  // highlighted marker on pause map (-1: none)
};

class MapView {
 public:
  // Reads colorado.nav, the map profiles and Horizon.zip's map textures
  // under `game_root`.
  bool open(const std::filesystem::path& game_root, std::string* error);
  bool ready() const { return !roads_.empty(); }

  // The minimap panel, size x size pixels.
  HudImage minimap(const MapScene& scene, int size) const;
  // The pause map panel; `zoom` 1 shows every road, larger zooms in on
  // (cx, cz).
  HudImage pause_map(const MapScene& scene, int width, int height, double zoom, double cx, double cz) const;
  // The middle of the road network (the pause map's first centre at zoom 1).
  std::array<double, 2> roads_centre() const { return {(min_x_ + max_x_) / 2, (min_z_ + max_z_) / 2}; }

  struct Rgba {
    float r = 255, g = 255, b = 255, a = 255;
  };
  struct LineStyle {
    double under_size = 0, line_size = 0;
    Rgba under, edge, core;  // the under-line; the line's ColourPrimary and ColourSecondary
  };
  struct IconStyle {
    int cell_x = 0, cell_y = 0;
    double size = 45;  // DynamicSize (or Size)
    Rgba colour;
  };
  struct Profile {
    std::map<std::string, LineStyle> roads;               // by road_type
    std::map<std::string, std::vector<IconStyle>> icons;  // by filter value, drawn in order
    Rgba region_fill{255, 255, 255, 255}, region_rim{0, 0, 0, 51};
  };
  struct Atlas {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
  };

 private:
  struct Road {
    int style = 0;  // index into kRoadTypes
    std::vector<std::array<double, 2>> points;
  };
  std::vector<Road> roads_;
  // A coarse grid of road segments for the minimap: (road, segment) by cell.
  double cell_ = 400, min_x_ = 0, min_z_ = 0, max_x_ = 0, max_z_ = 0;
  int nx_ = 0, nz_ = 0;
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> grid_;
  Profile mini_, full_;
  Atlas small_icons_, large_icons_, background_;
};

}  // namespace carview
