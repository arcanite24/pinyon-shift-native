// The hub rooms' own tracks (MENU-02, Q129): the Autoshow's uiautoshow, the
// workshop's uiworkshop and the paint shop's uipaintshop under media/tracks.
// Each is a small track folder like Colorado's whose render models all sit
// within 200 m of the origin. The title loads one behind a loading screen
// (LoadingDefs.xml has paintshop and workshop groups) and puts the car at its
// origin; the engine draws it as the world in Colorado's place while the
// room's screen is open (DriveWorldHost::enter_room_world).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include "hub_scene.h"
#include "showroom.h"

namespace carview {

// The room's track folder name, or null for the rooms without one (the
// garage, Barn Finds), which keep the studio stage.
const char* hub_room_track(HubRoom room);

// A room's texture records are its own track's (its .pvs texture table), so
// they are moved past Colorado's in the showroom's cache, one block per room.
inline constexpr uint32_t kHubRoomRecordBase = 0x40000000u;
uint32_t hub_room_record_base(HubRoom room);

// The room's turn about the car's origin (radians, about +y), taking the
// room's frame to the car's.
float hub_room_yaw(HubRoom room);

struct HubRoomScenery {
  HubRoom room = HubRoom::kNone;
  // The room's triangles about its origin (the game frame), texture records
  // moved by hub_room_record_base and their decoded textures attached.
  WorldTriangles triangles;
  size_t models = 0;      // render models gathered
  size_t textures = 0;    // texture records decoded
  size_t failed = 0;      // texture records that did not decode
  // The floor's height under the origin (the highest upward-facing floor
  // triangle there), where the car's wheels rest.
  float floor_y = 0.0f;
  // TrackSettings.xml's LightAmbient, LightHemiTop and LightHemiBottom (the
  // rooms keep fog off and have no sun: their light is baked in their
  // H_INT_*_LM lightmaps).
  std::array<float, 3> ambient{0.04f, 0.03f, 0.03f};
  std::array<float, 3> hemi_top{0.0f, 0.0f, 0.0f};
  std::array<float, 3> hemi_bottom{0.0f, 0.0f, 0.0f};
};

// The light a room's frame draws with (interim, MENU-02): no sun and no fog,
// and its lightmap, which load_hub_room puts in the AO slot so it scales
// the surface's colour, under a white hemisphere of this strength plus the
// room's own (TrackSettings.xml). The world shader adds a Light_Map as
// Colorado's night spill instead; the original's H_INT shaders are not
// decoded (Q222). 5 is measured on capture 20: the paint booth's back wall
// reads 150 of the original's 232 and its floor 135 of 144 (the tonemap
// compresses more gain away); the Autoshow's dome and the workshop look
// right at it.
inline constexpr float kHubRoomLightmapGain = 5.0f;

// Reads the room's track from `game_root`/media/tracks (the model index
// cached in `cache_dir`). False with `error` when the folder is missing or
// unreadable; the caller then keeps the studio stage.
bool load_hub_room(const std::filesystem::path& game_root, HubRoom room, const std::filesystem::path& cache_dir,
                   HubRoomScenery& out, std::string* error);

}  // namespace carview
