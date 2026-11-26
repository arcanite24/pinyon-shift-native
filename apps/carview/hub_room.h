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
  // H_INT_*_LM lightmaps, which world_frag.glsl's interior branch reads as
  // the decoded H_INT shaders do: albedo x c0.z x (LM + (AO.g + 0.05) x
  // ambColor), ambColor the image-based `ibl_ambient` below, not
  // LightAmbient). The car takes the static cube's mean
  // (`cube_mean`) as its ambient (interim, Q261).
  std::array<float, 3> ambient{0.04f, 0.03f, 0.03f};
  std::array<float, 3> hemi_top{0.0f, 0.0f, 0.0f};
  std::array<float, 3> hemi_bottom{0.0f, 0.0f, 0.0f};
  // The room's own post chain (TrackSettings.xml filmicTone, DesaturateAmount
  // and DesatLuminosity; Q222): the rooms keep every adaptive exposure key at
  // 0, so the exposure is filmicTone's, an EV (exposure 2^ev).
  bool tone = false;
  float hable[6] = {0.151f, 0.038f, 0.566f, 0.21f, 0.003f, 0.141f};
  float white = 1.5f;
  float exposure_ev = 0.75f;
  float desaturate = 0.92f;
  float luminance[3] = {0.2125f, 0.7154f, 0.0721f};
  // The car's light (imageBasedLighting and ShadowLightDirection): the key
  // light along `light_travel` (the game frame, the direction the light
  // travels) of colour iblSunColor x iblDirectScale x iblDirectScaleCar.
  std::array<float, 3> light_travel{0.0f, -1.0f, 0.0f};
  std::array<float, 3> car_key{0.63f, 0.63f, 0.63f};
  float ibl_ambient_scale = 1.0f;
  float ibl_ambient_saturation = 1.0f;
  // The mean radiance of the room's staticcarcubemap.xpr (LogLuv decoded).
  bool cube = false;
  std::array<float, 3> cube_mean{1.0f, 1.0f, 1.0f};
  // The image-based ambient: the cube's mean x iblAmbientScale x 2, its
  // colour at iblAmbientSaturation (the 2 fitted on capture 20).
  std::array<float, 3> ibl_ambient{1.0f, 1.0f, 1.0f};
};

// The room's post chain on top of the frame's (Q222): its tone curve,
// desaturation and fixed exposure; no LUT, vignette, contrast or the
// driving camera's optics. The popups' backdrop blur is kept.
void apply_hub_room_post(const HubRoomScenery& room, PostSettings& post);

// Reads the room's track from `game_root`/media/tracks. The model index is
// not cached on disk (a room has one to five models). False with `error`
// when the folder is missing or unreadable; the caller then keeps the
// studio stage.
bool load_hub_room(const std::filesystem::path& game_root, HubRoom room, HubRoomScenery& out, std::string* error);

}  // namespace carview
