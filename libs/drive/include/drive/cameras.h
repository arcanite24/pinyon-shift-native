// The driving cameras: two chase cameras, the cockpit, the hood and the
// front bumper, stepped through with one button, with the right stick's
// look around. Pure state and arithmetic: the caller gives the car's pose
// and motion each frame and gets an eye, a view and a lens back.
// Spec: SPEC-CAMERAS (163ffec3): the camera set, the per-car placement,
// the chase springs, the look rule, the field of view and the camera
// physics layers without noise, collision or impulse inputs.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "drive/head_tracking.h"

namespace drive {

// The driving group, in the cycle's order (the game's modes 0 to 4).
enum class CameraMode { kFollowLow = 0, kFollowHigh, kCockpit, kHood, kBumper };
constexpr int kCameraModes = 5;
const char* camera_mode_name(CameraMode m);

// Live Camera Editor overrides (Pinyon Shift F6 > DISPLAY > CAMERA EDITOR).
enum class DrivingView { kNormal = 0, kWheel = 1, kReplay = 2 };
const char* driving_view_name(DrivingView v);

struct CameraEditorConfig {
  double chase_height_m = 0.0;      // -0.50 .. +1.50 m
  double chase_distance_m = 0.0;    // -1.00 .. +3.00 m
  double chase_fov_scale = 1.0;     // 0.80 .. 1.30 (percentage of chase FOV)
  double chase_stiffness = 1.0;     // 0.50 .. 2.00 (K *= s, D *= sqrt(s) preserves damping ratio)
  DrivingView driving_view = DrivingView::kNormal;

  void reset() { *this = CameraEditorConfig{}; }
  bool is_default() const {
    return chase_height_m == 0.0 && chase_distance_m == 0.0 && chase_fov_scale == 1.0 &&
           chase_stiffness == 1.0 && driving_view == DrivingView::kNormal;
  }
};

// Immersive Camera layered over DriveCamera (F6 > DISPLAY > IMMERSIVE CAMERA).
enum class ImmersivePreset { kOff = 0, kSubtle = 1, kDefault = 2, kStrong = 3, kCustom = 4 };
const char* immersive_preset_name(ImmersivePreset p);
ImmersivePreset parse_immersive_preset(std::string_view text);

struct ImmersiveCameraConfig {
  double gforce = 0.0;  // 0.0 .. 2.0 (accel push-back, brake throw, onset jolts)
  double lean = 0.0;    // 0.0 .. 2.0 (corner roll & yaw lean; cockpit/hood)
  double shake = 0.0;   // 0.0 .. 2.0 (speed vibration & hard-braking judder; cockpit/hood)
  double head = 0.0;    // 0.0 .. 2.0 (cockpit breathing & head sway; cockpit/hood)

  void apply_preset(ImmersivePreset p);
  ImmersivePreset detect_preset() const;
};

// One CameraPhysics.xml layer (SPEC-CAMERAS, Camera physics layers):
// steady speed/g/pitch layers as well as bump, slip, collision jolt and
// noise shake layers.
struct CameraLayer {
  enum class Input {
    kSpeedMph,
    kSpeed,
    kConstantOne,
    kOneWhenStationary,
    kPitchDegrees,
    kGsLongitudinal,
    kGsLateral,
    kSlipLongitudinal,
    kSlipLateral,
    kSlip,
    kSurfaceBumpFL,
    kSurfaceBumpFR,
    kSurfaceBumpRL,
    kSurfaceBumpRR,
    kSuspensionBiasLongitudinal,
    kWorldCollision,
    kWorldCollisionLongitudinal,
    kWorldCollisionLateral,
    kCarCollision,
    kCarCollisionLongitudinal,
    kCarCollisionLateral,
    kRollDegrees,
    kGsVertical,
    kGs,
    kEngineNormRpm,
    kSteering,
    kYawRate,
    kSuspensionBiasLateral,
    kSuspensionBias,
    kSurfaceBump,
    kThrottle,
    kBrake,
    kDriftAngle,
    kGearShift,
    kBoost,
    kAirborne,
  };
  enum class Output {
    kCarXYZ,
    kCarYPR,
    kCameraXYZ,
    kCameraYPR,
    kCarXYZImpulse,
    kCarYPRImpulse,
    kCameraXYZImpulse,
    kCameraYPRImpulse,
    kTargetXYZ,
    kFov,
    kFovImpulse,
    kSteeringWheel,
  };
  enum class Mapping { kConstantOne, kLinear, kPoints };
  enum class Noise { kNone, kPerlin, kSimplex, kHeightGrid, kSin };
  Input input = Input::kConstantOne;
  Output output = Output::kFov;
  Mapping mapping = Mapping::kConstantOne;
  double clamp_min = -1e30, clamp_max = 1e30;
  bool take_derivative = false;
  bool mag_attack_decay = false;
  double input_attack = 0, input_decay = 0;
  std::vector<std::pair<double, double>> points;  // the curve, by input
  bool mirrored = false;
  Noise noise = Noise::kNone;
  Mapping noise_freq_mapping = Mapping::kConstantOne;
  std::vector<std::pair<double, double>> noise_freq_points;
  bool noise_freq_mirrored = false;
  double noise_freq = 1.0, noise_seed = 0.0;
  double noise_range_min = -1.0, noise_range_max = 1.0;
  std::array<double, 4> param{};  // x, y, z, w
  bool sprung = false;
  double spring_k = 0, spring_d_percent = 0;
  double impulse_cutoff = 0, impulse_delay = 0;
};

// CameraSettings.ini and CameraPhysics.xml.
struct CameraTuning {
  struct Chase {
    double height = 0, radius = 0, target = 0;  // the globals: HeightOffsetGlobal, RadiusOffsetGlobal, TargetYOffsetGlobal
    double height_min = 0, height_max = 0;      // HeightMin/MaxAboveGround
    double fov = 50, near_clip = 0.3;
    double yaw_k = 0, yaw_d = 0, yaw_car_damp = 0, yaw_max_speed = 2;
    double height_k = 0, height_d = 0;
  };
  std::array<Chase, 2> chase;  // FollowLowCam, FollowHighCam
  double driver_fov = 62, driver_near = 0.1;
  std::array<double, 3> driver_offset{0, 0.21, -0.51};  // Driver\DefaultOffsetFromSteeringWheel
  double hood_fov = 66, hood_near = 0.1;
  std::array<double, 3> hood_default{0, 0, 0};  // HoodCam\CamYOffset, CamZOffset, CamPitchOffset
  double bumper_fov = 53, bumper_near = 0.3, bumper_z = 0, bumper_rest_height = 1.15;
  std::array<std::vector<CameraLayer>, kCameraModes> layers;
  std::array<int, kCameraModes> layers_left_out{};  // enabled layers not modelled
};
bool parse_camera_settings(std::string_view ini, CameraTuning& out, std::string* error);
bool parse_camera_physics(std::string_view xml, CameraTuning& out, std::string* error);

// The car's own inputs, in SPEC-CAMERAS' car frame (x right, y up, z
// forward, the model origin; the ground at minus the ride height at rest).
struct CameraCar {
  std::array<double, 3> box_min{}, box_max{};  // MAXData's collision box, z already shifted
  std::array<double, 3> com{};                 // the centre of mass
  std::array<double, 3> steering_wheel{};      // CarPartPositions PartId 9
  std::map<std::string, double> overrides;     // CameraOverrides columns (missing: 0)
  bool has_cockpit_mesh = true;                // false skips kCockpit in cycle/set_mode (CAMERAS.md)
  double override(const std::string& column) const;
};

// What the camera reads of the car each frame (the world: y up).
struct CameraInput {
  // The car frame to the world (column-major; rotation and translation).
  double car_to_world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  double speed = 0;      // m/s
  double heading = 0;    // the car's world yaw, atan2(forward x, forward z)
  double yaw_rate = 0;   // rad/s
  double contact = 1;    // the mean ground contact of the four wheels (0-1)
  double pitch_degrees = 0;  // the car's pitch, nose up positive
  // The body's acceleration along its forward and right axes in g (9.81
  // m/s^2), gravity left out; forward positive speeding up (SPEC-CAMERAS,
  // Inputs).
  double g_longitudinal = 0, g_lateral = 0;
  // Each wheel's ground point's height in the car frame (LF, RF, LR, RR).
  std::array<double, 4> wheel_ground_y{};
  // Motion-effect and jolt layer inputs (SPEC-CAMERAS, Camera physics layers).
  double slip_longitudinal = 0, slip_lateral = 0;
  std::array<double, 4> surface_bump{};  // FL, FR, RL, RR surface/suspension bump
  double suspension_bias_longitudinal = 0;
  double world_collision_longitudinal = 0, world_collision_lateral = 0;
  double car_collision_longitudinal = 0, car_collision_lateral = 0;
  // Extended CameraPhysics.xml inputs & SurfaceTypes.xml ChaseCamSpringMultiplier.
  double chase_cam_spring_mult = 1.0;
  double roll_degrees = 0;
  double g_vertical = 0;
  double engine_norm_rpm = 0;
  double steer_input = 0;
  double suspension_bias_lateral = 0;
  double throttle = 0, brake = 0;
  double drift_angle_degrees = 0;
  double shift_impulse = 0;
  double boost_norm = 0;
  // The world's height under a point (for the chase clearance); empty: none.
  std::function<bool(double x, double y, double z, double& ground_y)> ground_below;
  // The world's ceiling height above a point (for tunnel/underpass clearance);
  // empty: falls back to probing upward via obstacle_ray when present.
  std::function<bool(double x, double y, double z, double& ceiling_y)> ceiling_above;
  // Horizontal wall / car-OBB obstacle raycast from look_at toward chase eye;
  // returns true and writes hit_dist in [0, max_dist] if blocked (CAMERAS.md:266).
  std::function<bool(const std::array<double, 3>& origin, const std::array<double, 3>& dir, double max_dist,
                     double& hit_dist)>
      obstacle_ray;
};

// Oriented bounding box of a nearby AI or traffic vehicle for chase-camera
// obstacle pull-in so FollowLow/FollowHigh never clip inside trucks, buses,
// or trailing opponents.
struct CameraVehicleObb {
  std::array<double, 3> center{};
  std::array<double, 3> axis_x{1, 0, 0};
  std::array<double, 3> axis_y{0, 1, 0};
  std::array<double, 3> axis_z{0, 0, 1};
  std::array<double, 3> half_extents{0.95, 0.72, 2.25};
};

enum class CameraObstacleSource : uint8_t { kTraffic = 0, kFestivalAi = 1, kRaceAi = 2 };

// Publishes the current frame's vehicle OBBs for a simulation source.
void set_camera_vehicle_obbs(CameraObstacleSource source, std::vector<CameraVehicleObb> obbs);
void clear_camera_vehicle_obbs(CameraObstacleSource source);

// Ray-vs-OBB slab intersection in world space. Returns true and writes
// hit_dist in [0, max_dist] on hit.
bool intersect_ray_obb(const std::array<double, 3>& origin, const std::array<double, 3>& dir, double max_dist,
                       const CameraVehicleObb& obb, double& hit_dist);

// Intersects a ray against all published vehicle OBBs within `max_player_dist`
// of `player_pos`. Returns true and writes the nearest hit_dist in [0, max_dist].
bool intersect_camera_vehicle_obbs(const std::array<double, 3>& origin, const std::array<double, 3>& dir,
                                   double max_dist, const std::array<double, 3>& player_pos,
                                   double max_player_dist, double& hit_dist);

struct CameraView {
  std::array<double, 3> eye{}, forward{0, 0, 1}, up{0, 1, 0};  // world
  double fov_degrees = 50, near_clip = 0.3;  // vertical
  CameraMode mode = CameraMode::kFollowLow;
  bool interior = false;  // the cockpit: the game draws the interior there
  double steering_wheel_kick = 0.0;  // CameraPhysics.xml SteeringWheel layer output (rad)
};

class DriveCamera {
 public:
  void set_tuning(const CameraTuning& t) { tuning_ = t; }
  void set_car(const CameraCar& c);
  const CameraCar& car() const { return car_; }
  CameraMode mode() const { return mode_; }
  void set_mode(CameraMode m);
  void cycle();  // SwitchCamera: the next mode, wrapping
  // The look stick's target: 0 ahead, pi back, +pi/2 right, -pi/2 left.
  void set_look(double angle) { look_target_ = angle; }
  // Live Camera Editor, Immersive Camera, and OpenTrack head tracking layers.
  void set_camera_editor(const CameraEditorConfig& cfg) { editor_ = cfg; }
  const CameraEditorConfig& camera_editor() const { return editor_; }
  void set_immersive_camera(const ImmersiveCameraConfig& cfg) { immersive_ = cfg; }
  const ImmersiveCameraConfig& immersive_camera() const { return immersive_; }
  void set_head_pose(const HeadPose& pose) { head_pose_ = pose; }
  const HeadPose& head_pose() const { return head_pose_; }
  // Starts the current camera again (a reset or a new car).
  void restart() {
    started_ = false;
    ++restart_count_;
  }
  uint64_t restart_count() const { return restart_count_; }
  void clear_pending_restart() {
    if (has_started_once_) started_ = true;
  }
  // Steps the springs by dt (a dt over half a second settles them) and
  // places the camera.
  CameraView update(double dt, const CameraInput& in);

  // Measured constants (SPEC-CAMERAS: d_base, the look-at drop, the hood
  // anchor, the bumper's box inset).
  static constexpr double kBaseDistance[2] = {2.95, 3.10};
  // The chase eye's fixed lift over the box top, L (code constants).
  static constexpr double kEyeLift[2] = {0.35, 0.60};
  static constexpr double kHoodAnchor[3] = {0, 0.45, 0.60};
  static constexpr double kBoxInset = 0.6096;
  static constexpr double kClearance = 0.3;
  static constexpr double kCeilingClearance = 0.25;
  static constexpr double kFanHalfWidth = 0.35;
  // SPEC-CAMERAS (Q51): the slope term's limit as a multiple of the
  // distance and how long it stays on once fewer than three wheels touch;
  // the look spring's time constant is ours (open).
  static constexpr double kSlopeLimit = 2.0, kSlopeHold = 0.2, kLookTime = 0.1;

 private:
  struct LayerState {
    double x = 0, v = 0;
    double prev_in = 0;
    double env = 0, env_sign = 1.0;
    double noise_phase = 0;
    double impulse_timer = -1.0, pending_impulse = 0.0;
    double impulse_cooldown = 0.0, prev_impulse_mag = 0.0;
    bool started = false;
  };
  struct LayerOut {
    std::array<double, 3> car_xyz{}, car_ypr{}, camera_xyz{}, camera_ypr{}, target_xyz{};
    double fov = 0;
    double steering_wheel = 0;  // rad
  };
  LayerOut step_layers(double dt, const CameraInput& in);
  LayerOut step_immersive(double dt, const CameraInput& in);
  CameraView chase(double dt, const CameraInput& in, const LayerOut& l);
  CameraView on_car(const CameraInput& in, const LayerOut& l);
  CameraView special_view(const CameraInput& in, const LayerOut& l);
  void start(const CameraInput& in);

  CameraTuning tuning_;
  CameraCar car_;
  CameraMode mode_ = CameraMode::kFollowLow;
  CameraEditorConfig editor_;
  ImmersiveCameraConfig immersive_;
  HeadPose head_pose_;
  bool started_ = false;
  bool has_started_once_ = false;
  uint64_t restart_count_ = 0;
  std::vector<LayerState> layer_state_;
  // Immersive camera runtime springs/phases.
  double imm_g_long_ = 0, imm_g_lat_ = 0;
  double imm_prev_g_long_ = 0, imm_jolt_ = 0, imm_jolt_v_ = 0;
  double imm_phase_ = 0;
  double look_ = 0, look_target_ = 0;
  // Chase state.
  // slope_hold_: seconds since three or more wheels last touched.
  double look_old_ = 0;  // the look angle the eye stood at last frame
  double theta_ = 0, omega_ = 0, h_ = 0, h_rate_ = 0, c_ = 0, slope_hold_ = 0;
  double yaw_rate_smooth_ = 0;  // slew-damped car yaw rate for crash-shock immunity
  double wall_pull_ = 1.0;  // damped horizontal obstacle pull-in fraction [0.2, 1.0]
  // The bumper's ground height, taken when it starts.
  double bumper_ground_ = 0;
};

}  // namespace drive
