// Shared math, camera, input, I/O, font-cache, and world-texture helpers for
// carview's showroom and drive runtimes.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "cars/bake_format.h"
#include "drive/cameras.h"
#include "drive/controls.h"
#include "drive/from_bake.h"
#include "drive/save.h"
#include "drive/vehicle.h"
#include "drive_helpers.h"
#include "fh1data/archive.h"
#include "fh1data/ui_overlay.h"
#include "fh1data/ui_scene.h"
#include "freeroam/collectibles.h"
#include "game_hud.h"
#include "hud.h"
#include "race/career.h"
#include "race_view.h"
#include "renderer.h"
#include "showroom.h"
#include "world/collision.h"
#include "world/world.h"

namespace carview {

inline constexpr float kDrivePi = 3.14159265358979f;

struct V3 {
  float x, y, z;
};
inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross3(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 norm(V3 a) { return mul(a, 1.0f / std::sqrt(dot3(a, a))); }

void multiply4(const float* a, const float* b, float* out);
void invert_affine(const float* m, float* out);
V3 transform_v3(const float* m, V3 p, float w);
void rotate_direction(const float m[16], const float v[3], float out[3]);

// Studio lighting overrides (--light name=value,... in showroom mode).
struct LightSettings {
  float exposure = 0.4f, environment = 0.586f, key = 0.076f, fill = 1.0f;
  float tint[3] = {0.88f, 0.9f, 0.95f};
  float reflect[3] = {0.512f, 0.9f, 1.775f};
  float metallic = 0.273f;
};

bool parse_light(const std::string& text, LightSettings& out);
void lighting(ShowroomFrame& f, bool livery, const LightSettings& light = {});

// Last camera projection parameters captured by look_frame() for Renderer passes
// (replaces file-scope mutable globals).
struct CameraProjectionState {
  float near_z = 0.25f;
  float far_z = 16000.0f;
  float proj00 = 1.0f;
  float proj11 = -1.0f;
  float vp_unjittered[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// Explicit cache owned by DriveApp / CLI tools for UI4 texture & scene archives
// (replaces function-local static caches in load_ui_fonts()).
struct UiFontCache {
  struct State {
    // The disc's UI archives under the title update's media.zip.
    std::shared_ptr<fh1::UiOverlay> overlay;
    std::set<std::string> failed_textures;
    std::filesystem::path rally_tex_dir;
    bool rally_probed = false;
    std::map<std::string, std::unique_ptr<fh1::UiScene>> scene_cache;
  };
  std::shared_ptr<State> state = std::make_shared<State>();
};

bool write_wav(const std::filesystem::path& path, int rate, const std::vector<float>& stereo);
bool write_png(const std::string& path, uint32_t w, uint32_t h, int channels, const uint8_t* data);
bool read_png(const std::string& path, int channels, uint32_t w, uint32_t h, std::vector<uint8_t>& out,
              std::string& error);
std::string mask_path_for(const std::string& out);
// The UI fonts, and the texture and scene resolvers over the disc and, when
// `state_root` holds title-update-v4/media.zip, the title update's copies.
bool load_ui_fonts(const std::filesystem::path& game_root, UiFontCache& cache,
                   const std::filesystem::path& state_root = {});
void upload_ui_atlas(Renderer& renderer, bool full = false);
bool write_menu_png(const HudState& st, const std::string& path, const CareerBoard* board = nullptr);

std::vector<std::string> scan_lut_files(const std::filesystem::path& state_root);
bool apply_user_cube_lut(const std::string& lut_name, const std::filesystem::path& state_root, Renderer& renderer,
                         std::string& error);

ShowroomFrame look_frame(V3 eye, V3 target, V3 up, float aspect, bool livery, bool reverse_z = false,
                         float fov_degrees = 55.0f, float near_clip = 0.25f,
                         CameraProjectionState* cam_out = nullptr, const LightSettings& light = {});

struct CameraPull {
  double fraction = 1;
  bool started = false;
  double yaw = 0;
  double look = 0;
  double look_target = 0;
};

std::string lower_ascii(std::string v);
double angle_to(double a, double b);
double deadzone(double v, double inside, double outside);

ShowroomFrame chase_frame(const cars::bake::CarInfo& car, const drive::Vehicle& v, float aspect, bool front,
                          const drive::Ground* ground = nullptr, CameraPull* pull = nullptr, double dt = 0,
                          bool reverse_z = false, CameraProjectionState* cam_out = nullptr);
ShowroomFrame scenic_orbit_frame(const cars::bake::CarInfo& car, const drive::Vehicle& v, float aspect, double yaw_rad,
                                 float dist, float height, bool reverse_z, const drive::Ground* ground = nullptr,
                                 CameraProjectionState* cam_out = nullptr);
bool project_world_point(const ShowroomFrame& frame, const cars::bake::CarInfo& car, const drive::Vehicle& v, double wx,
                         double wy, double wz, float screen_w, float screen_h, float& sx, float& sy);

std::string sdl_key_name(SDL_Keycode k);
bool action_key_down(const bool* keys, const drive::KeyBindings& kb, drive::ControlAction act);
drive::PadControl pad_control_from_button(Uint8 b);
double pad_control_value(SDL_Gamepad* pad, drive::PadControl c, double dz_in, double dz_out);
const char* difficulty_name(race::Difficulty d);
const char* difficulty_label(race::Difficulty d);
race::DifficultyAssists to_difficulty_assists(const drive::AssistSettings& assists, int damage_mode = 1);
void apply_vehicle_assists(drive::Vehicle& vehicle, const drive::AssistSettings& assists);
bool load_camera_rig(const std::filesystem::path& game_root, const std::filesystem::path& bake_dir,
                     const cars::bake::CarInfo& info, const drive::Vehicle& v, drive::DriveCamera& rig,
                     std::string& note, bool camera_effects = true, bool has_cockpit = true);
ShowroomFrame camera_view_frame(const drive::CameraView& view, const cars::bake::CarInfo& info,
                                const drive::Vehicle& v, float aspect, bool reverse_z,
                                CameraProjectionState* cam_out = nullptr);
void apply_ndc_jitter(ShowroomFrame& f, float jx, float jy);
void compute_clip_from_world(const ShowroomFrame& f, const cars::bake::CarInfo& info, const drive::Vehicle& v,
                             float out[16]);
ShowroomFrame rig_frame(drive::DriveCamera& rig, const cars::bake::CarInfo& info, const drive::Vehicle& v,
                        const drive::Ground* ground, RigAccelState& accel, double dt, float aspect, bool reverse_z,
                        bool* interior, double* com_distance = nullptr, drive::CameraView* out_view = nullptr,
                        const world::Collision* collision = nullptr, double car_coll_long = 0.0,
                        double car_coll_lat = 0.0, std::array<double, 4>* prev_wheel_comp = nullptr,
                        CameraProjectionState* cam_out = nullptr);

float brake_glow(const drive::Vehicle& v);
void wheel_poses(const drive::Vehicle& v, WheelPose poses[4]);

struct WorldTextureLoader {
  std::set<uint32_t> failed;
  std::set<uint32_t> cutout;
  size_t decoded = 0;

  void load(world::World& colorado, WorldTriangles& w, Showroom& showroom,
            std::vector<uint32_t>& pin_records);
};

inline constexpr float kSunGame[3] = {0.3848f, 0.8754f, 0.2926f};
inline constexpr double kTreeRadius = 2000;
inline constexpr double kModelRadius = 900, kPropRadius = 600, kRegather = 200, kSwapLag = 50;
inline constexpr double kLowModelRadius = 600, kLowPropRadius = 500;
inline constexpr double kPropLod0 = 120, kPropLodBand = 25, kPropEdgeBand = 40, kSwapFadeSeconds = 0.5;
inline constexpr double kPostExposure = 0.8;
inline constexpr float kPostBalance[3] = {1.10f, 0.88f, 0.88f};

std::map<int64_t, std::string> baked_car_ids(const std::filesystem::path& bake_dir);

struct Orbit {
  float yaw = 145.0f, pitch = 12.0f, distance = 0;
};

ShowroomFrame orbit_frame(const Showroom& s, const Orbit& o, float aspect, bool livery,
                          const LightSettings& light = {});

extern const char* kDriveRuns[6][2];

WorldTriangles scenery(const world::Mesh& mesh, double origin_x, double origin_z);

void scenery_transform(const cars::bake::CarInfo& car, const drive::Vehicle& v, double origin_x, double origin_z,
                       float out[16]);
void read_post_settings(const std::filesystem::path& world_dir, PostSettings& s);
bool read_shadow_direction(const std::filesystem::path& world_dir, float travel[3]);
void progress_from_save(const drive::DriveSave& s, const freeroam::Collectibles& data, freeroam::Progress& p);
void progress_to_save(const freeroam::Collectibles& data, const freeroam::Progress& p, drive::DriveSave& s);
std::string barn_car_label(const freeroam::Barn& b);

}  // namespace carview
