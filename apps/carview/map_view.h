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
#include "freeroam/road_discovery.h"
#include "hud.h"

namespace carview {

enum class MapFilter : int {
  kAll = 0,
  kEvents,
  kStreetRaces,
  kOutposts,
  kCollectibles,
  kPrStunts,
  kCount
};

inline const char* map_filter_name(MapFilter f) {
  switch (f) {
    case MapFilter::kAll: return "ALL";
    case MapFilter::kEvents: return "EVENTS";
    case MapFilter::kStreetRaces: return "STREET";
    case MapFilter::kOutposts: return "OUTPOSTS";
    case MapFilter::kCollectibles: return "COLLECTIBLES";
    case MapFilter::kPrStunts: return "PR STUNTS";
    default: return "ALL";
  }
}

struct MapMarker {
  enum Kind { kEvent, kStreetHub, kFestivalSite, kOutpost, kPrStunt, kSpeedTrap, kSpeedZone, kH2hFinish };
  Kind kind = kEvent;
  std::string icon_tag;  // MapProfile key: race, exhibition, streetrace, nemesisrace, racecentral, autoshow, workshop, paintshop, gas_station, ...
  std::string id;        // HorizonEventID, hub id, site name, outpost id, or stunt id
  std::string title;
  std::string subtitle;
  double x = 0, z = 0;
  bool is_new = false;
  bool completed = false;
  bool discovered = true;  // false for undiscovered outposts
  int site_or_hub = 0;  // FestivalSite::Kind, StreetHub::id, or Outpost index
};

struct MapScene {
  double x = 0, z = 0;  // the player, game frame
  double heading = 0;   // rad, 0 faces +z (north), positive turns towards +x (east)
  double speed_mps = 0; // player speed (m/s) for dynamic minimap zoom
  bool north_up = false;  // true: north-up fixed minimap with rotating player arrow
  const freeroam::Collectibles* data = nullptr;
  const freeroam::Progress* progress = nullptr;
  const freeroam::RoadDiscovery* road_discovery = nullptr;
  // Unfound signs and barns' exact places on the map (the game's Treasure
  // Map, or its collectibles cheat); found ones and rumours always show.
  bool reveal = false;
  MapFilter filter = MapFilter::kAll;
  // A race's opponents (x, z), drawn as dots.
  std::vector<std::array<double, 2>> opponents;
  // Free-roam traffic vehicles (x, z), drawn as subtle car dots on the minimap.
  std::vector<std::array<double, 2>> traffic;
  // Roaming Festival AI racers (x, z), drawn as distinct amber/pink racer discs.
  std::vector<std::array<double, 2>> festival_ai;
  // A race's route ahead (x, z), drawn green on the minimap (SPEC-UI-LAYOUTS a).
  std::vector<std::array<double, 2>> route;
  // Active free-roam GPS route (x, z) and destination point.
  std::vector<std::array<double, 2>> gps_route;
  bool has_gps_dest = false;
  double gps_dest_x = 0, gps_dest_z = 0;
  // Unlocked career events, Street Hubs, Festival sites, Outposts & PR Stunts.
  std::vector<MapMarker> markers;
  int selected_marker = -1;  // highlighted marker on pause map (-1: none)
};

class MapView {
 public:
  // Reads colorado.nav, the map profiles and Horizon.zip's map textures
  // under `game_root` (loading localized MapIconSheet for `lang` with EN fallback).
  bool open(const std::filesystem::path& game_root, std::string* error, const std::string& lang = "EN");
  bool ready() const { return !roads_.empty(); }

  // The minimap panel, size x size pixels.
  HudImage minimap(const MapScene& scene, int size) const;
  // The pause map panel; `zoom` 1 shows every road, larger zooms in on
  // (cx, cz).
  HudImage pause_map(const MapScene& scene, int width, int height, double zoom, double cx, double cz) const;
  // Metres per pixel at the given viewport size and zoom (matches pause_map).
  double map_scale(int width, int height, double zoom) const;
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

  // Returns how many of the 15 high-res map tiles are currently cached in RAM.
  int loaded_high_tiles() const;

 private:
  void ensure_high_tiles(int c0, int c1, int r0, int r1) const;
  bool sample_high_pixel(int px, int py, uint8_t out_rgb[3]) const;

  struct Road {
    int style = 0;  // index into kRoadTypes
    std::vector<uint32_t> nodes;  // colorado.nav node indices
    std::vector<std::array<double, 2>> points;
    double min_x = 0, max_x = 0, min_z = 0, max_z = 0;
  };
  struct PauseBaseCache {
    int width = 0, height = 0, pad = 0;
    double scale = 0.0;
    double cx = 0.0, cz = 0.0;
    const freeroam::RoadDiscovery* road_disc = nullptr;
    int road_disc_count = -1;
    size_t road_visited_nodes = 0;
    size_t road_visited_segs = 0;
    uint32_t loaded_tiles_mask = 0;
    std::vector<uint8_t> rgba;
  };
  uint32_t high_tiles_mask() const;
  void render_pause_base(std::vector<uint8_t>& rgba, int w, int h, double scale, double hi_blend, double cx, double cz,
                         const freeroam::RoadDiscovery* road_discovery) const;

  std::vector<Road> roads_;
  // A coarse grid of road segments for the minimap: (road, segment) by cell.
  double cell_ = 400, min_x_ = 0, min_z_ = 0, max_x_ = 0, max_z_ = 0;
  int nx_ = 0, nz_ = 0;
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> grid_;
  Profile mini_, full_;
  Atlas small_icons_, large_icons_, background_;
  std::filesystem::path horizon_zip_path_;
  static constexpr int kHighCols = 5, kHighRows = 3, kHighTileSize = 1024;
  mutable std::array<Atlas, 15> high_tiles_{};
  mutable std::array<uint8_t, 15> high_tile_state_{};  // 0 unloaded, 1 loaded, 2 failed
  mutable PauseBaseCache base_cache_;
};

// Smooth, dt-integrated fullscreen map camera controller: continuous
// velocity with smooth acceleration/deceleration, exponential zoom
// interpolation, and gentle magnetic snap to nearby visible markers.
class MapCameraController {
 public:
  void reset(double cx, double cz, double zoom = 2.0);
  void nudge_target_zoom(double factor);
  void pan_direct(double dx, double dz);
  // Advances cursor position and zoom smoothly by `dt` seconds.
  // `pan_x` (+east), `pan_z` (+north) in [-1, 1], `zoom_rate` in [-1, 1].
  // Returns true if position or zoom changed noticeably (needing redraw).
  bool step(double dt, double pan_x, double pan_z, double zoom_rate,
            const std::vector<MapMarker>& visible_markers);

  double cx() const { return cx_; }
  double cz() const { return cz_; }
  double vx() const { return vx_; }
  double vz() const { return vz_; }
  double zoom() const { return zoom_; }
  double target_zoom() const { return target_zoom_; }
  int snapped_marker() const { return snapped_marker_; }

 private:
  void clamp_bounds();
  double cx_ = 0.0, cz_ = 0.0;
  double vx_ = 0.0, vz_ = 0.0;
  double zoom_ = 2.0, target_zoom_ = 2.0;
  int snapped_marker_ = -1;
};

}  // namespace carview
