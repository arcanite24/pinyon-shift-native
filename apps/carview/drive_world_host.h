// Outer world & Vulkan renderer host for `carview --drive` (survives car swaps,
// paint changes, upgrades, and race starts/leaves without tearing down Vulkan
// pipelines, WorldGpu, world textures, World, Collision, or Streamer).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio_host.h"
#include "drive/vehicle.h"
#include "drive_app.h"
#include "drive_helpers_runtime.h"
#include "frame_inspector.h"
#include "freeroam/collectibles.h"
#include "hub_room.h"
#include "render_modern.h"
#include "renderer.h"
#include "showroom.h"
#include "world/collision.h"
#include "world/road_follow.h"
#include "world/stream.h"
#include "world/vfx.h"
#include "world/world.h"

namespace carview {

struct DriveWorldHost {
  struct Prepared {
    std::shared_ptr<Showroom::StagedWorld> staged;
    world::RoadFollower::Points road;
    std::vector<world::WorldGlow> glows;
  };

  static constexpr int kAudioRate = AudioHost::kAudioRate;

  DriveApp& app;
  Renderer renderer;
  Showroom showroom{renderer};
  CameraProjectionState cam_proj;

  float shadow_light_game[3] = {0, 1, 0};
  bool have_shadow_light = false;

  bool user_lut_active = false;
  bool perf_overlay_open = false;
  bool inspector_open = false;
  InspectorView inspector_view = InspectorView::kColor;
  uint32_t temporal_frame_idx = 0;
  double last_frame_cpu_ms = -1.0;
  std::array<float, 4> inspector_preview_uv{};
  bool has_inspector_preview = false;

  modern::GraphicsSettings active_graphics;
  modern::FramePacer frame_pacer;
  int display_hz = 60;

  world::ParticleSystem particle_sys;
  world::SkidMarkSystem skid_sys;
  uint32_t last_audio_backfire_count = 0;
  int last_vfx_gear = 1;
  double last_impact_vfx_time = -10.0;

  drive::FlatGround flat;
  const drive::Ground* ground = &flat;
  world::World colorado;
  world::Collision collision;
  world::GatherOptions world_gather_options;
  int loaded_world_route = 0;
  std::unique_ptr<world::Streamer> streamer;
  std::unique_ptr<freeroam::FreeRoam> free_roam;
  std::shared_ptr<const std::set<std::string>> hidden_sent;
  WorldTextureLoader texture_loader;
  world::RoadFollower follower;
  drive::Vec3 start{};
  double start_heading = 0;
  bool streamer_async = false;
  bool first_session = true;
  std::shared_ptr<Prepared> deferred_prepared_;

  // MENU-02: the hub room whose own track (hub_room.h) is the drawn world in
  // Colorado's place while its screen is open; kNone draws Colorado. The
  // streamer keeps gathering Colorado meanwhile, but its swaps wait.
  HubRoom room_world = HubRoom::kNone;
  std::atomic<bool> prepare_on_main{false};  // the streamer's prepare run by leave_room_world
  std::map<HubRoom, std::shared_ptr<const HubRoomScenery>> room_scenery;
  std::set<HubRoom> room_failed;
  // Draws `room`'s track (read once, then kept). False when the room has no
  // track or it does not read: the caller keeps the studio stage.
  bool enter_room_world(HubRoom room);
  // Draws Colorado again around the streamer's last gather.
  bool leave_room_world();
  const HubRoomScenery* room_world_scenery() const {
    auto it = room_scenery.find(room_world);
    return it != room_scenery.end() ? it->second.get() : nullptr;
  }

  explicit DriveWorldHost(DriveApp& a)
      : app(a),
        perf_overlay_open(a.cli_perf_overlay),
        inspector_open(a.cli_frame_inspector),
        inspector_view(a.cli_inspector_view),
        active_graphics(a.cli_graphics),
        last_audio_backfire_count(a.audio_host.telemetry().backfire_count),
        follower(a.cruise_kmh / 3.6),
        streamer_async(!a.headless || a.async_stream) {}

  ShowroomOptions build_car_options();
  bool set_staged_world();
  bool pump_deferred_world();
  bool open_world_and_car();
  bool hot_reload_car_and_world();
  void run();
};

}  // namespace carview
