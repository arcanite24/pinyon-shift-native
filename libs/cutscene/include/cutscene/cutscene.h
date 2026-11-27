// FH1 in-engine cutscenes: the CutsceneManager documents in
// gamemodes.zip (<track>/Cutscenes/cutscenes_*.xml).
//
// A cutscene is a timeline of camera cuts ("Cam", each with position,
// rotation and FOV keys, depth-of-field and post-effect keys and optional
// fade-in / fade-out options) plus a list of timed event triggers (camera
// target nodes, car placement and release, audio, voice-over, mixer
// snapshots, animations). This library parses the documents and evaluates a
// cutscene at a time t; it is headless (no renderer, no audio).
//
// Evaluation rules (read from the original's camera player, 2026-11-25):
// - A cam's local time maps to a fractional key index through a cubic
//   Hermite over the (key time, key index) samples. Interior tangents are the
//   central difference of the index over time, the end tangents the parabolic
//   end condition (3 x the end secant minus the neighbour tangent, halved);
//   with two keys the map is linear.
// - The pose at a fractional key index is a uniform Catmull-Rom spline over
//   the keys: interior tangents 0.5 x (next - previous), the first and last
//   tangents one-sided (next - this). Every channel (position, yaw, pitch,
//   roll, FOV) is interpolated independently in raw degrees.
// - Frames: positions are in the frame's body axes (+X right, +Y up, +Z
//   forward). Yaw is positive to the right about +Y and pitch positive
//   down, as the driving cameras (inferred from the data: the cockpit cams
//   sit at x < 0, the left-hand driver's seat; cams framing a placed car
//   keep it in view under this convention and lose it under the mirrored
//   one; cams set high above the car carry positive pitch). Roll is
//   applied positive counter-clockwise seen from behind (top leans left,
//   matching the game's CarSpaceYPROffset z convention and original diner /
//   festival-outro captures). A cam with a TargetName is relative
//   to that camera target node (placed by a CCutsceneCameraNodeTrigger);
//   without one it is relative to the player car. Node and car "rotation"
//   attributes are headings in degrees: forward is (sin r, 0, cos r).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cutscene {

enum class PlaybackMode : uint8_t { kNormal, kLooping };
enum class CurveType : uint8_t { kLinear, kS };
enum class AnimSpace : uint8_t { kCarSpace, kPartSpace, kTargetPart, kUnknown };
enum class DofMode : uint8_t { kOff, kManual, kOther };

// A FadeInOptions / FadeOutOptions / FadeOptions element.
struct FadeOptions {
  double duration = 0.0;
  double hold = 0.0;  // HoldDuration: the colour is held this long first
  CurveType curve = CurveType::kLinear;
  std::array<float, 4> color{};  // Color.x..w (every disc fade is black)
  bool active() const { return duration > 0.0 || hold > 0.0; }
};

struct CamKey {
  double time = 0.0;
  std::array<double, 3> pos{};
  double yaw = 0.0, pitch = 0.0, roll = 0.0;  // degrees
  double fov = 50.0;                          // degrees (vertical)
  double time_c0 = 0.0;                       // zero in every disc key
  std::string car_part;                       // PartSpace keys only
};

struct PostKey {
  double time = 0.0;
  double focal_distance = 0.0, focal_length = 0.0, focal_max_blur = 0.0;
  double saturation = 0.0, exposure = 0.0, brightness = 0.0, contrast = 0.0, sepia = 0.0, blur = 0.0;
};

struct Cam {
  double start = 0.0;     // StartCut, seconds into the cutscene
  double duration = 0.0;  // the cut's length
  std::string target_name;
  // Type="Follow" (the race's prerace_countdown): the cut hands the view to
  // the game's own driving camera; it has no keys.
  bool follow = false;
  bool disable_csm = false, disable_cubemap = false, disable_car_rendering = false;
  bool apply_motion_blur = false;
  AnimSpace pos_space = AnimSpace::kCarSpace;
  AnimSpace rot_space = AnimSpace::kCarSpace;
  std::string rot_part_target;  // RotAnimPartToTarget (e.g. "steering")
  std::string time_anim_type;   // "AffectsPath" on every disc cam
  bool in_cockpit = false;
  double ease_in = 0.0, ease_out = 0.0, const_vel_speed = 0.0;  // zero on every disc cam
  std::vector<CamKey> keys;
  DofMode dof = DofMode::kOff;
  double motion_blur = 0.0;
  std::vector<PostKey> post_keys;
  FadeOptions fade_in, fade_out;
  int physics_layers = 0;
  double end() const { return start + duration; }
};

enum class EventType : uint8_t {
  kCameraNode,       // CCutsceneCameraNodeTrigger: a named frame for cams
  kPlacePlayerCar,   // CCutscenePlaceCarTrigger
  kCar,              // CCutsceneCarTrigger: spawn a cutscene car (CarID)
  kReleaseCar,       // CCutsceneReleaseCarTrigger: hand a cutscene car to the AI
  kAudio,            // CCutsceneAudioTrigger: a UI sound event (group/event)
  kVoiceOver,        // CCutsceneVoiceOverTrigger
  kMixSnapshot,      // CCutsceneAudioMixSnapshotTrigger
  kMusic,            // CCutsceneMusicTrigger
  kShowDriver,       // CCutsceneShowDriverTrigger (hide / show drivers)
  kAnimation,        // CCutsceneAnimationTrigger (animated props / people)
  kCountdown,        // CCutsceneCountdownTrigger
  kStartCountdown,   // CCutsceneStartCountdownTrigger
  kBarnFindHud,      // CCutsceneShowBarnFindHUDSceneTrigger
  kNemesisUi,        // CCutsceneNemesisUITrigger
  kOther,
};

EventType event_type_from_id(std::string_view id);
const char* event_type_name(EventType type);

struct Event {
  double time = 0.0;
  EventType type = EventType::kOther;
  std::string id;    // the trigger class as written
  std::string name;  // the trigger's name attribute
  std::vector<std::pair<std::string, std::string>> attributes;  // every attribute, in order

  std::string attr(std::string_view key, std::string fallback = {}) const;
  double number(std::string_view key, double fallback = 0.0) const;
  bool has(std::string_view key) const;
  // x/y/z and rotation (degrees), for the placing triggers.
  std::array<double, 3> position() const { return {number("x"), number("y"), number("z")}; }
  double rotation_deg() const { return number("rotation"); }
};

struct Cutscene {
  std::string name;
  PlaybackMode mode = PlaybackMode::kNormal;
  double duration = 0.0;
  double loop_from = 0.0;
  bool mirror_for_rhd = false;
  int declared_cams = 0;  // Cams NumCams (0 when there is no Cams element)
  std::vector<Cam> cams;
  std::vector<Event> events;  // sorted by time (stable)

  // The cam covering t (start <= t < end); the last cam that started before
  // t when t falls in a gap or past the end; -1 when there are no cams.
  int cam_at(double t) const;
};

struct Library {
  std::vector<Cutscene> cutscenes;
  const Cutscene* find(std::string_view name) const;  // case-insensitive
};

// Parses a CutsceneManager document. Unknown elements and attributes are
// ignored. Returns false with `error` on malformed XML or a document whose
// root is not CutsceneManager.
bool parse_cutscenes(std::string_view xml, Library& out, std::string* error);

// The animated cameras' sequences in media/camera.zip (CUI4AnimatedCamera):
// a document whose root is one <Cams> (HomeSpaceCams.xml, carrevealcams.xml;
// one looping sequence named "") or whose root holds named <Cutscene>
// elements (WorkshopCams.xml's PartSpecificCameraSequences). Each sequence
// loops; its length is its Duration, else its ResetPlayback cam's start
// (that cam is dropped); a cam of zero duration runs to the next cam.
bool parse_camera_sequences(std::string_view xml, Library& out, std::string* error);

// The free-roam cutscenes of the player's disc: media/gamemodes.zip
// Colorado/Cutscenes/cutscenes_default_freeroam.xml under `game_root` (the
// Rally expansion ships a byte-identical copy).
inline constexpr const char* kFreeRoamCutscenesMember = "Colorado/Cutscenes/cutscenes_default_freeroam.xml";
bool load_freeroam_cutscenes(const std::filesystem::path& game_root, Library& out, std::string* error);

// The race cutscenes (the flows' CPrepareCutscene): the default set, and a
// track's own set under Tracks/ (cutscenes_<Tracks.DevName>.xml), whose
// cutscenes are named <name>_<DevName>.
inline constexpr const char* kRaceCutscenesMember = "Colorado/Cutscenes/cutscenes_default_race.xml";
std::string track_cutscenes_member(std::string_view dev_name);
// Appends the cutscenes of one gamemodes.zip member to `out`.
bool load_gamemodes_cutscenes(const std::filesystem::path& game_root, std::string_view member, Library& out,
                              std::string* error);
// A race_specific cutscene: the track's <name>_<DevName> when it has one,
// else the default <name>.
const Cutscene* find_race_cutscene(const Library& lib, std::string_view name, std::string_view dev_name);

// ---- Evaluation ----

// The fractional key index for local time t (clamped to the key range).
double key_parameter(const std::vector<CamKey>& keys, double t);

struct Pose {
  std::array<double, 3> pos{};
  double yaw = 0.0, pitch = 0.0, roll = 0.0, fov = 50.0;
};
// The Catmull-Rom pose at fractional key index u.
Pose pose_at_parameter(const std::vector<CamKey>& keys, double u);
// The pose of a cam at its local time t.
Pose sample_pose(const Cam& cam, double local_t);

struct PostSample {
  bool dof = false;
  double focal_distance = 0.0, focal_length = 0.0, focal_max_blur = 0.0;
  double saturation = 0.0, exposure = 0.0, brightness = 0.0, contrast = 0.0, sepia = 0.0, blur = 0.0;
  double motion_blur = 0.0;
};
// Post-effect keys are interpolated linearly between keys (interim: the
// original's post-effect interpolation is not traced) and held past the ends.
PostSample sample_post(const Cam& cam, double local_t);

// The S curve used by FadeOptions (smoothstep 3x^2 - 2x^3); Linear is x.
double fade_curve(CurveType curve, double x);

// A full-screen colour overlay: rgb plus coverage alpha (0 = clear).
struct Overlay {
  std::array<float, 3> rgb{};
  float alpha = 0.0f;
};
// The overlay a FadeOptions produces at time `t` since it started, fading in
// (colour to clear) or out (clear to colour).
Overlay fade_overlay(const FadeOptions& fade, double t, bool fade_in);
// The cutscene's own fades at time t: each cam's FadeInOptions from its start,
// its FadeOutOptions over the last `duration` before its end.
Overlay cutscene_overlay(const Cutscene& cs, double t);

struct Sample {
  int cam_index = -1;
  double local_time = 0.0;
  Pose pose;
  PostSample post;
  Overlay overlay;
};
// Evaluates the cutscene at time t (seconds from its start, not wrapped).
Sample sample(const Cutscene& cs, double t);

// ---- World placement ----

struct Frame {
  std::array<double, 3> origin{};
  double heading_deg = 0.0;  // forward = (sin h, 0, cos h)
};

struct WorldCamera {
  std::array<double, 3> eye{};
  std::array<double, 3> forward{0, 0, 1};
  std::array<double, 3> up{0, 1, 0};
  double fov_deg = 50.0;
  bool interior = false;
};

// The world camera of a pose expressed in `frame`.
WorldCamera to_world(const Pose& pose, const Frame& frame, bool in_cockpit = false);

// PartSpace cams on the player car are relative to one of its parts
// (headlightL/R, taillightL/R, wheelLF/RF/LR/RR on the disc): the part's
// position in the car's body axes (+X right, +Y up, +Z forward, origin at
// the car's ground point), keeping the car's heading. A PartSpace cam on a
// camera target node ignores its part (the node has none).
struct CarPart {
  std::string name;
  std::array<double, 3> pos{};
};
using CarParts = std::vector<CarPart>;
// The part (case-insensitive); false when the car has no such part.
bool find_part(const CarParts& parts, std::string_view name, std::array<double, 3>& out);
// The frame of a car part on a car frame.
Frame part_frame(const Frame& car, const std::array<double, 3>& part_pos);

// CCutscenePlaceCarTrigger's initialSpeed is in miles per hour (the
// original converts it with 0.44704 before setting the car's velocity).
inline constexpr double kMphToMps = 0.44704;
inline double place_speed_mps(const Event& e) { return e.number("initialSpeed") * kMphToMps; }

// ---- Playback ----

// A cutscene being played: advances time, fires each event once as the
// playhead crosses it, tracks camera target nodes and resolves each cam's
// frame (its target node, or the player car).
class Player {
 public:
  void start(const Cutscene* cs);
  void stop();
  // Steps by dt. Returns the events whose time was crossed (in time order).
  std::vector<const Event*> step(double dt);

  bool active() const { return cs_ != nullptr && !finished_; }
  bool finished() const { return finished_; }
  const Cutscene* cutscene() const { return cs_; }
  double time() const { return t_; }

  // Hold for the voice-over: a cutscene with an exitCutsceneOnFinish
  // voice-over (Wristband_Intro) loops from LoopFrom to the end while that
  // voice-over plays and ends when it finishes. Held, the loop lasts while
  // the voice-over is pending (up to kVoiceStartGrace after its trigger) or
  // playing; a voice-over that never starts lets the cutscene finish at its
  // Duration. Looping cutscenes always loop.
  static constexpr double kVoiceStartGrace = 1.5;
  void set_hold(bool hold) { hold_ = hold; }
  bool hold() const { return hold_; }
  // The host's voice-over state, before each step.
  void set_voice_playing(bool playing);
  bool holding() const;

  // The evaluated sample at the current (wrapped) time.
  Sample current() const;
  // The frame a cam is relative to, given the player car's frame: its target
  // node (resolved by name when the cut runs: a node trigger already fired,
  // else the cutscene's trigger of that name), or the player car, offset to
  // the part the cam's key at `local_t` names for a PartSpace cam.
  Frame frame_for(const Cam& cam, const Frame& player_car, const CarParts* parts = nullptr,
                  double local_t = 0.0) const;
  // The current world camera (false when not active, the cutscene has no
  // cams, or the current cut is a Follow cut).
  bool world_camera(const Frame& player_car, WorldCamera& out, const CarParts* parts = nullptr) const;
  const std::vector<std::pair<std::string, Frame>>& nodes() const { return nodes_; }
  // Named frames of the scene the cutscene plays in (the race route's
  // start_location_00, post_race_location_00, ...): a cam's target node
  // when the cutscene places none of that name. Kept across start().
  void set_scene_nodes(std::vector<std::pair<std::string, Frame>> nodes) { scene_nodes_ = std::move(nodes); }

  // Ground-height query used to resolve `snapToGround="1"` camera target nodes
  // (e.g. Festival_Race_Outro's CameraTargetNode_0) onto the world road surface.
  // Kept across start().
  using GroundQuery = std::function<bool(double x, double y_hint, double z, double& out_y)>;
  void set_ground_query(GroundQuery query) { ground_query_ = std::move(query); }

  // Local ground-surface probe for non-cockpit cutscene eye clearance: should
  // only report surfaces near `y_hint` (e.g. within +2.5 m above) so overhead
  // bridges/arches are not hit. If a surface is found, the exterior camera eye
  // is clamped at least `kMinEyeGroundClearance` above it. Kept across start().
  static constexpr double kMinEyeGroundClearance = 0.20;
  void set_eye_clearance_query(GroundQuery query) { eye_clearance_query_ = std::move(query); }

  // Vertical offset from the player car's ground-contact point (`player_car.origin[1]`)
  // to the car's CarSpace model origin (`car.lift`). Applied to CarSpace cams
  // targeting the car (`TargetName == ""`), not to PartSpace cams (whose parts
  // are already in ground-relative car body coordinates) or world target nodes.
  void set_car_body_offset_y(double offset_y) { car_body_offset_y_ = offset_y; }
  double car_body_offset_y() const { return car_body_offset_y_; }

  // Optional override for the car frame used by `TargetName == ""` cams (e.g.
  // New_Diner_Cutscene_2 framing the parked Corrado before the player swap).
  // Cleared automatically on start() and stop().
  void set_car_frame_override(std::optional<Frame> frame) { car_frame_override_ = frame; }

  // A placing trigger's frame (a camera node, a PlacePlayerCar): its x/y/z
  // and rotation, which are in its parent's frame when it names one (Skills_Unlock's
  // CameraTargetNode_0 sits on PlacePlayerCar_0), else the world's.
  Frame event_frame(const Event& e) const;

 private:
  struct NodeMeta {
    std::string name;
    Frame raw;
    bool snap_to_ground = false;
    const Event* event = nullptr;
  };

  void fire_range(double t0, double t1, bool inclusive_start, std::vector<const Event*>& out);
  Frame event_frame(const Event& e, int depth, bool apply_snap = true) const;
  Frame snap_frame(const Frame& f, bool snap_to_ground) const;

  const Cutscene* cs_ = nullptr;
  double t_ = 0.0;
  bool started_ = false;
  bool finished_ = false;
  bool hold_ = false;
  enum class ExitVo : uint8_t { kNone, kFired, kPlaying, kDone } exit_vo_ = ExitVo::kNone;
  double exit_vo_wait_ = 0.0;
  std::vector<bool> fired_;
  std::vector<std::pair<std::string, Frame>> nodes_;
  std::vector<NodeMeta> node_meta_;
  std::vector<std::pair<std::string, Frame>> scene_nodes_;
  GroundQuery ground_query_;
  GroundQuery eye_clearance_query_;
  double car_body_offset_y_ = 0.0;
  std::optional<Frame> car_frame_override_;
};

}  // namespace cutscene
