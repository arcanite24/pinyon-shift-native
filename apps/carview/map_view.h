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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace carview {

// R3-UI-12: Computes the (x, y) rim position for the rotating "N" compass badge on a square minimap.
// In north-up mode (`map_heading_rad == 0`), North sits dead-top at (size/2, size/2 - rim_r).
inline std::array<double, 2> minimap_compass_north_pos(double map_heading_rad, int size) {
  const double half = 0.5 * double(size);
  const double rim_r = std::max(8.0, half - std::max(10.0, double(size) * 0.065));
  const double a = -map_heading_rad;
  return {half + rim_r * std::sin(a), half - rim_r * std::cos(a)};
}

}  // namespace carview

#ifndef CARVIEW_MAP_VIEW_LOGIC_ONLY

#include "freeroam/collectibles.h"
#include "freeroam/road_discovery.h"
#include "hud.h"
#include "map_anim.h"
#include "map_filter.h"

namespace fh1 {
class Archive;
}

namespace carview {

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
  std::string detail_line;  // optional rich event / PR stunt metadata line
  // The pause map's popup (map_screen.h): its heading and second line (an
  // outpost's or hub's name, a place's line, a speed trap's PB), an event's
  // class letter, points and prize, and a pip per race or mission
  // (MapPip: -1 locked, 0 open, 1..3 gold, silver, bronze).
  std::string popup_line;
  std::string class_name;
  std::string points, money;
  std::vector<int> pips;
  std::string discount;  // an outpost's "0%"
  // Event/hub icon status metadata (1:1 with MapProfileFullscreen / MapProfileMinimap):
  int best_place = 0;         // 0 = not on podium, 1 = gold, 2 = silver, 3 = bronze
  bool eligible = true;       // false -> show yellow '!' warning badge (*_career_note)
  bool is_circuit = false;    // race: laps > 1 -> circuit icon (2,0) instead of P2P (1,0)
  bool is_finale = false;     // Horizon Finale (style 6 / career_type 12) -> crown icon (7,1)
  int wristband_level = -1;   // -1 = qualifier/white, 0..6 = Yellow..Gold
  std::array<uint8_t, 4> ring_rgba{0, 0, 0, 0};  // optional explicit RGBA ring tint (a > 0 overrides level)
};

// Lightweight status bundle passed to MapView::resolve_icon_layer (shared by pause_map and minimap).
struct MapIconState {
  bool completed = false;
  bool discovered = true;
  bool eligible = true;
  int best_place = 0;
  bool is_circuit = false;
  bool is_finale = false;
  int wristband_level = -1;
  std::array<uint8_t, 4> ring_rgba{0, 0, 0, 0};
};

inline MapIconState map_icon_state(const MapMarker& m) {
  MapIconState st;
  st.completed = m.completed;
  st.discovered = m.discovered;
  st.eligible = m.eligible;
  st.best_place = m.best_place;
  st.is_circuit = m.is_circuit;
  st.is_finale = m.is_finale;
  st.wristband_level = m.wristband_level;
  st.ring_rgba = m.ring_rgba;
  return st;
}

// The marker's filter row (map_filter.h); PR stunts have no icon.
MapCategory map_marker_category(const MapMarker& m);
bool map_marker_matches_filter(const MapMarker& m, const MapFilterSet& f);

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
  MapFilterSet filter;
  // A race's opponents (x, z), drawn as dots.
  std::vector<std::array<double, 2>> opponents;
  // Free-roam traffic vehicles (x, z), drawn as subtle car dots on the minimap.
  std::vector<std::array<double, 2>> traffic;
  // Roaming Festival AI racers (x, z), drawn as distinct amber/pink racer discs.
  std::vector<std::array<double, 2>> festival_ai;
  // A race's route ahead (x, z), drawn green on the minimap (SPEC-UI-LAYOUTS a).
  std::vector<std::array<double, 2>> route;
  // R3-UI-12: Minimap race overlays (next checkpoint gate, checkered finish line, AI race leader).
  bool has_next_checkpoint = false;
  std::array<double, 2> next_checkpoint{};
  bool has_finish = false;
  std::array<double, 2> finish_pos{};
  bool has_race_leader = false;
  std::array<double, 2> race_leader{};
  // Active free-roam GPS route (x, z) and destination point.
  std::vector<std::array<double, 2>> gps_route;
  bool has_gps_dest = false;
  double gps_dest_x = 0, gps_dest_z = 0;
  // Unlocked career events, Street Hubs, Festival sites, Outposts & PR Stunts.
  std::vector<MapMarker> markers;
  int selected_marker = -1;  // highlighted marker on pause map (-1: none)
  // The pause map behind the FILTER popup and the fast travel dialogs:
  // blurred (map_filter_orig.png, map_fasttravel_confirm_orig.png).
  bool blur = false;
  // The pause map's icon animations (MapProfileFullscreen.xml, map_anim.h):
  // each icon plays its symbolizers' "intro" from the map's opening, "hover"
  // under the cursor while the rest play "ghost", the rest "unghost" once
  // the cursor leaves, and "default" where a symbolizer binds nothing to its
  // event. Times are seconds since the map opened; anim_time < 0 draws each
  // event's last frame (still pictures, tests).
  double anim_time = -1;
  bool has_hover = false;       // an icon under the cursor at (hover_x, hover_z)
  double hover_x = 0, hover_z = 0;
  double hover_since = 0;       // when this hover, or the last one's end, began
  double ghost_since = -1;      // when the others ghosted (-1: not yet this opening)
  bool has_prev_hover = false;  // the icon hovered before (it ghosts from hover_since)
  double prev_hover_x = 0, prev_hover_z = 0;
};

class MapView {
 public:
  MapView();
  ~MapView();
  MapView(MapView&&) noexcept;
  MapView& operator=(MapView&&) noexcept;

  // Reads colorado.nav, the map profiles and Horizon.zip's map textures
  // under `game_root` (loading localized MapIconSheet for `lang` with EN fallback).
  bool open(const std::filesystem::path& game_root, std::string* error, const std::string& lang = "EN");
  bool ready() const { return !roads_.empty(); }

  // The minimap panel, size x size pixels.
  HudImage minimap(const MapScene& scene, int size) const;
  // The pause map panel; `zoom` >= min_zoom fits strictly inside the authored
  // Colorado map bounds with zero black borders, larger zooms in on (cx, cz).
  HudImage pause_map(const MapScene& scene, int width, int height, double zoom, double cx, double cz) const;
  // Minimum zoom factor for (width, height) that keeps the full viewport inside
  // the Colorado map texture bounds (zero black borders at any aspect ratio).
  double min_zoom(int width, int height) const;
  // Metres per pixel at the given viewport size and zoom (matches pause_map).
  double map_scale(int width, int height, double zoom) const;
  // Clamps (cx, cz) so the visible viewport at (width, height, zoom) stays
  // strictly within Colorado's authored map texture bounds.
  void clamp_camera(int width, int height, double zoom, double& cx, double& cz) const;
  // Internal raster dimensions used by pause_map (capped <= 1600x900 for smooth 120 FPS panning).
  static std::array<int, 2> pause_internal_size(int width, int height);
  // The middle of the road network (the pause map's first centre at zoom 1).
  std::array<double, 2> roads_centre() const { return {(min_x_ + max_x_) / 2, (min_z_ + max_z_) / 2}; }

  struct Rgba {
    float r = 255, g = 255, b = 255, a = 255;
  };
  struct LineStyle {
    double under_size = 0, line_size = 0;
    Rgba under, edge, core;  // the under-line; the line's ColourPrimary and ColourSecondary
  };
  enum class IconRole {
    kBase,          // drop shadow or standalone icon (always drawn)
    kCareerBase,    // career activity base icon (*_career, nemesis (4,1), streetrace (2,1))
    kCareerColour,  // wristband tier ring (*_career_colour: race (0,0), nemesis (5,0))
    kCareerFinish,  // completion trophy cup (*_career_finish: (5,1); drawn only when completed)
    kCareerNote,    // ineligible '!' warning badge (*_career_note: (6,1); drawn only when !eligible)
  };
  struct IconStyle {
    int cell_x = 0, cell_y = 0;
    double size = 45;  // DynamicSize (or Size)
    Rgba colour;
    IconRole role = IconRole::kBase;
    int sort_order = 0;
    mapanim::Bindings anims;  // event -> animation name
    // VisibleRange (m from the player): shown to max_fade_distance, faded
    // out by max_distance; 0 = no range (always shown).
    double max_distance = 0, max_fade_distance = 0;
  };
  struct ResolvedIconLayer {
    bool visible = false;
    int cell_x = 0, cell_y = 0;
    Rgba colour;
  };
  // Classifies a MapProfile <Symbolizer> into its 1:1 status role.
  static IconRole classify_icon_role(std::string_view symbolizer_type, std::string_view group_key, int cell_x,
                                     int cell_y);
  // Retail CareerWristbandLevels ring colour for wristband tier (-1 white, 0..6 Yellow..Gold).
  static Rgba wristband_ring_colour(int wristband_level, std::array<uint8_t, 4> explicit_rgba);
  // Completion trophy tint for best podium place (1 gold, 2 silver, 3 bronze; 0 defaults to gold).
  static Rgba trophy_colour(int best_place);
  // Resolves visibility, atlas cell, and colour for a single symbolizer layer (used by pause_map and minimap).
  static ResolvedIconLayer resolve_icon_layer(const IconStyle& style, std::string_view group_key,
                                              const MapIconState& state);
  // True when a marker should display the pink NEW badge (never on completed markers or non-career icons).
  static bool marker_shows_new_badge(std::string_view group_key, bool is_new, bool completed);
  // A symbolizer's VisibleRange alpha at `dist` m (1 inside, 0 past max).
  static float visible_range_alpha(const IconStyle& s, double dist);
  const std::vector<IconStyle>* fullscreen_icon_styles(std::string_view key) const;
  const std::vector<IconStyle>* minimap_icon_styles(std::string_view key) const;
  struct Profile {
    std::map<std::string, LineStyle> roads;               // by road_type
    std::map<std::string, std::vector<IconStyle>> icons;  // by filter value, drawn in order
    Rgba region_fill{255, 255, 255, 255}, region_rim{0, 0, 0, 51};
    mapanim::Library anims;       // the profile's <animations>
    mapanim::Bindings new_badge;  // the career icons' "NEW" symbolizer (icon_up_career_new)
  };
  struct Atlas {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
  };

  // Returns how many of the 15 high-res map tiles are currently cached in RAM.
  int loaded_high_tiles() const;
  // True when retail New.xds badge texture was loaded from Horizon.zip.
  bool has_new_badge() const { return !new_badge_.rgba.empty(); }
  const Atlas& large_icons() const { return large_icons_; }
  // The pause map profile's animations: one by name (the camera moves), and
  // the one the `key` group's first symbolizer binding `event` plays.
  const mapanim::Library& animations() const { return full_.anims; }
  const mapanim::Animation* animation(std::string_view name) const { return full_.anims.find(name); }
  const mapanim::Animation* icon_animation(std::string_view key, std::string_view event) const;
  const Atlas& small_icons() const { return small_icons_; }

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
  struct UpscaleLut {
    int src_w = 0, dst_w = 0;
    std::vector<int> x0, x1;
    std::vector<uint16_t> fx;
  };
  uint32_t high_tiles_mask() const;
  void render_pause_base(std::vector<uint8_t>& rgba, int w, int h, double scale, double hi_blend, double cx, double cz,
                         const freeroam::RoadDiscovery* road_discovery) const;
  void upscale_pause_rgba(const std::vector<uint8_t>& src, int sw, int sh,
                          std::vector<uint8_t>& dst, int dw, int dh) const;

  std::vector<Road> roads_;
  // A coarse grid of road segments for the minimap: (road, segment) by cell.
  double cell_ = 400, min_x_ = 0, min_z_ = 0, max_x_ = 0, max_z_ = 0;
  // The part of the background texture that is painted (world metres); its
  // edges are black and the pause map's camera stays off them.
  double art_west_ = freeroam::kMapWest, art_east_ = freeroam::kMapWest + freeroam::kMapWidth;
  double art_south_ = freeroam::kMapNorth - freeroam::kMapHeight, art_north_ = freeroam::kMapNorth;
  void find_art_bounds();
  int nx_ = 0, nz_ = 0;
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> grid_;
  Profile mini_, full_;
  Atlas small_icons_, large_icons_, background_, new_badge_;
  std::filesystem::path horizon_zip_path_;
  mutable std::shared_ptr<fh1::Archive> horizon_zip_;
  static constexpr int kHighCols = 5, kHighRows = 3, kHighTileSize = 1024;
  mutable std::array<Atlas, 15> high_tiles_{};
  mutable std::array<uint8_t, 15> high_tile_state_{};  // 0 unloaded, 1 loaded, 2 failed
  mutable PauseBaseCache base_cache_;
  mutable std::vector<uint8_t> work_rgba_;
  mutable UpscaleLut upscale_lut_;
};

// Smooth, dt-integrated fullscreen map camera controller: continuous
// velocity with smooth acceleration/deceleration, exponential zoom
// interpolation, and gentle magnetic snap to nearby visible markers.
class MapCameraController {
 public:
  void set_viewport(int width, int height, const MapView* view = nullptr);
  double min_zoom() const;
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
  int view_w_ = 1600, view_h_ = 900;
  const MapView* map_view_ = nullptr;
};

}  // namespace carview

#endif  // CARVIEW_MAP_VIEW_LOGIC_ONLY
