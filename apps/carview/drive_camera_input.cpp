#include "drive_helpers_runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "cars/mods.h"
#include "drive/from_physics.h"
#include "render_modern.h"

namespace carview {

// A camera at eye looking at target, in the car's model frame.
ShowroomFrame look_frame(V3 eye, V3 target, V3 up, float aspect, bool livery, bool reverse_z,
                         float fov_degrees, float near_clip, CameraProjectionState* cam_out,
                         const LightSettings& light) {
  fov_degrees = carview::modern::effective_vfov(fov_degrees, aspect);
  const float fovy = fov_degrees * kDrivePi / 180.0f;
  const V3 f = norm(sub(target, eye));
  const V3 side = norm(cross3(f, up));
  const V3 u = cross3(side, f);
  const float view[16] = {side.x, u.x, -f.x, 0, side.y, u.y, -f.y, 0, side.z, u.z, -f.z, 0,
                          -dot3(side, eye), -dot3(u, eye), dot3(f, eye), 1};
  const float near_z = near_clip, far_z = 16000.0f;  // the far ring reaches several km
  const float t = 1.0f / std::tan(fovy / 2);
  float projection[16] = {};
  projection[0] = t / aspect;
  projection[5] = -t;  // Vulkan clip: +y down
  projection[10] = far_z / (near_z - far_z);
  projection[11] = -1;
  projection[14] = near_z * far_z / (near_z - far_z);
  if (reverse_z) {
    // Reversed, with the far plane at infinity: depth = near / distance.
    projection[10] = 0;
    projection[14] = near_z;
  }
  ShowroomFrame frame{};
  multiply4(projection, view, frame.view_projection);
  if (cam_out) {
    cam_out->near_z = near_z;
    cam_out->far_z = far_z;
    cam_out->proj00 = projection[0];
    cam_out->proj11 = projection[5];
    std::memcpy(cam_out->vp_unjittered, frame.view_projection, sizeof cam_out->vp_unjittered);
  }
  std::memcpy(frame.transparent_view_projection, frame.view_projection, sizeof frame.view_projection);
  frame.eye[0] = eye.x;
  frame.eye[1] = eye.y;
  frame.eye[2] = eye.z;
  frame.eye[3] = 1;
  lighting(frame, livery, light);
  return frame;
}

// Our chase camera (placeholder values until a camera spec): behind and
// above the car, following its heading but not its pitch and roll, so the
// body's motion on its springs shows. Built in the world and taken into the
// model frame, where the showroom draws.
// --view front: a fixed front three-quarter view from the car's left, low,
// for checking the wheels' steer and spin.
// The chase camera's pull-in (ours, until the camera spec): a ray from the
// look-at point to the eye against the ground's collision; when it meets a
// surface the eye moves in to 0.2 m short of it (at once), and eases back
// out over about a third of a second once clear. `pull` keeps the fraction
// of the full distance between frames (1 = full); `dt` is the time since
// the last frame.
// The chase camera's state between frames (ours): how far it is pulled in
// by the collision, its heading, which trails the car's, and a look-around
// angle from the right stick.

std::string lower_ascii(std::string v) {
  for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
  return v;
}

// The signed angle from a to b, in (-pi, pi].
double angle_to(double a, double b) {
  const double d = std::remainder(b - a, 2 * 3.14159265358979323846);
  return d;
}

// A stick or trigger value rescaled from its inside to its outside deadzone
// to 0..1 and clamped, the sign kept (CAR_PHYSICS "Throttle, brake and
// steering from the pad": the rule is probable; the deadzone values are
// ours, ForzaProfile's defaults are not read).
double deadzone(double v, double inside, double outside) {
  const double m = std::clamp((std::fabs(v) - inside) / (outside - inside), 0.0, 1.0);
  return v < 0 ? -m : m;
}

ShowroomFrame chase_frame(const cars::bake::CarInfo& car, const drive::Vehicle& v, float aspect, bool front,
                          const drive::Ground* ground, CameraPull* pull, double dt, bool reverse_z,
                          CameraProjectionState* cam_out) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(car, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  const drive::Vec3 p = v.position();
  const drive::Vec3 fwd = drive::rotate(v.orientation(), {0, 0, 1});
  const double car_yaw = std::atan2(fwd.x, fwd.z);
  // The chase heading trails the car's (time constant 0.2 s, ours) so a
  // slide or spin does not whip the view; the look angle eases to its
  // target in 0.1 s. Headless frames (dt of a second or more) settle.
  double yaw_d = car_yaw;
  bool first_pull = false;
  if (pull) {
    if (!pull->started) {
      pull->yaw = car_yaw;
      pull->look = pull->look_target;
      pull->started = true;
      first_pull = true;
    } else if (dt > 0) {
      pull->yaw += angle_to(pull->yaw, car_yaw) * (1 - std::exp(-dt / 0.2));
      pull->look += (pull->look_target - pull->look) * (1 - std::exp(-dt / 0.1));
    }
    yaw_d = pull->yaw + pull->look;
  }
  const float yaw = float(front ? car_yaw : yaw_d);
  const V3 dir{std::sin(yaw), 0, std::cos(yaw)};
  const V3 base{float(p.x), float(p.y), float(p.z)};
  const V3 left{-dir.z, 0, dir.x};
  // Distance and height from the car's size (ours): 4.5 m back and 1.6 m
  // up for a 4.3 m, 1.3 m car.
  const auto& size = v.params().size;
  const float back = float(1.9 + 0.6 * size.z), up = float(0.6 + 0.77 * size.y);
  const V3 eye = front ? add(add(add(base, mul(dir, 4.0f)), mul(left, 3.0f)), V3{0, 0.9f, 0})
                       : add(add(base, mul(dir, -back)), V3{0, up, 0});
  const V3 target = front ? add(base, V3{0, 0.5f, 0})
                          : add(add(base, mul(dir, float(0.35 * size.z))), V3{0, 0.7f, 0});
  V3 eye_used = eye;
  if (ground && pull) {
    if (dt > 0 || first_pull) {
      const drive::Vec3 from{target.x, target.y, target.z};
      const drive::Vec3 ray{double(eye.x) - target.x, double(eye.y) - target.y, double(eye.z) - target.z};
      const double reach = std::sqrt(ray.x * ray.x + ray.y * ray.y + ray.z * ray.z);
      double want = 1;
      drive::Hit hit;
      if (reach > 1e-6 && ground->raycast(from, ray * (1 / reach), reach, hit))
        want = std::clamp((hit.distance - 0.2) / reach, 0.1, 1.0);
      pull->fraction =
          want < pull->fraction ? want : pull->fraction + (want - pull->fraction) * std::min(1.0, std::max(0.0, dt) * 3);
    }
    const float f = float(pull->fraction);
    eye_used = add(target, mul(V3{eye.x - target.x, eye.y - target.y, eye.z - target.z}, f));
  }
  return look_frame(transform_v3(world_to_model, eye_used, 1), transform_v3(world_to_model, target, 1),
                    norm(transform_v3(world_to_model, {0, 1, 0}, 0)), aspect, false, reverse_z, 55.0f, 0.25f, cam_out);
}

// Scenic orbiting camera around the car (Title Screen backdrop & Pre-Race Grid showcase).
// Raycasts against `ground` when supplied so the camera never clips inside a building or wall.
ShowroomFrame scenic_orbit_frame(const cars::bake::CarInfo& car, const drive::Vehicle& v, float aspect, double yaw_rad,
                                 float dist, float height, bool reverse_z, const drive::Ground* ground,
                                 CameraProjectionState* cam_out) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(car, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  const drive::Vec3 p = v.position();
  const V3 base{float(p.x), float(p.y), float(p.z)};
  const V3 target{base.x, base.y + 0.65f, base.z};
  V3 eye{base.x + dist * float(std::sin(yaw_rad)), base.y + height, base.z + dist * float(std::cos(yaw_rad))};
  if (ground) {
    const drive::Vec3 from{target.x, target.y, target.z};
    const drive::Vec3 ray{double(eye.x) - target.x, double(eye.y) - target.y, double(eye.z) - target.z};
    const double reach = std::sqrt(ray.x * ray.x + ray.y * ray.y + ray.z * ray.z);
    drive::Hit hit;
    if (reach > 1e-6 && ground->raycast(from, ray * (1.0 / reach), reach, hit)) {
      const float frac = float(std::clamp((hit.distance - 0.35) / reach, 0.25, 1.0));
      eye = add(target, mul(V3{eye.x - target.x, eye.y - target.y, eye.z - target.z}, frac));
    }
  }
  return look_frame(transform_v3(world_to_model, eye, 1), transform_v3(world_to_model, target, 1),
                    norm(transform_v3(world_to_model, {0, 1, 0}, 0)), aspect, false, reverse_z, 45.0f, 0.25f, cam_out);
}

// Projects a world-frame point through the player car's current view_projection to screen px.
bool project_world_point(const ShowroomFrame& frame, const cars::bake::CarInfo& car, const drive::Vehicle& v, double wx,
                         double wy, double wz, float screen_w, float screen_h, float& sx, float& sy) {
  float m2w[16], w2m[16];
  drive::model_to_world(car, v, m2w);
  invert_affine(m2w, w2m);
  const V3 pm = transform_v3(w2m, {float(wx), float(wy), float(wz)}, 1.0f);
  const float* vp = frame.view_projection;
  const float cx = vp[0] * pm.x + vp[4] * pm.y + vp[8] * pm.z + vp[12];
  const float cy = vp[1] * pm.x + vp[5] * pm.y + vp[9] * pm.z + vp[13];
  const float cw = vp[3] * pm.x + vp[7] * pm.y + vp[11] * pm.z + vp[15];
  if (cw <= 0.1f) return false;
  sx = (cx / cw * 0.5f + 0.5f) * screen_w;
  sy = (cy / cw * 0.5f + 0.5f) * screen_h;
  return true;
}

const char* difficulty_name(race::Difficulty d) {
  switch (d) {
    case race::Difficulty::kEasy: return "EASY";
    case race::Difficulty::kMed: return "MED";
    case race::Difficulty::kHard: return "HARD";
    case race::Difficulty::kPro: return "PRO";
    case race::Difficulty::kHardPlus: return "HARD+";
  }
  return "MED";
}

SDL_Scancode sdl_scancode_for(std::string_view name) {
  if (name.empty()) return SDL_SCANCODE_UNKNOWN;
  if (name == "Comma") return SDL_SCANCODE_COMMA;
  if (name == "Period") return SDL_SCANCODE_PERIOD;
  if (name == "LeftShift") return SDL_SCANCODE_LSHIFT;
  if (name == "RightShift") return SDL_SCANCODE_RSHIFT;
  if (name == "LeftCtrl") return SDL_SCANCODE_LCTRL;
  if (name == "RightCtrl") return SDL_SCANCODE_RCTRL;
  return SDL_GetScancodeFromName(std::string(name).c_str());
}

std::string sdl_key_name(SDL_Keycode k) {
  switch (k) {
    case SDLK_COMMA: return "Comma";
    case SDLK_PERIOD: return "Period";
    case SDLK_LSHIFT: return "LeftShift";
    case SDLK_RSHIFT: return "RightShift";
    case SDLK_LCTRL: return "LeftCtrl";
    case SDLK_RCTRL: return "RightCtrl";
    default: break;
  }
  const char* raw = SDL_GetKeyName(k);
  if (!raw || !*raw) return {};
  std::string out;
  for (const char* p = raw; *p; ++p)
    if (*p != ' ') out.push_back(*p);
  return out;
}

bool action_key_down(const bool* keys, const drive::KeyBindings& kb, drive::ControlAction act) {
  const auto& b = kb.map[size_t(act)];
  const SDL_Scancode s1 = sdl_scancode_for(b.primary);
  if (s1 != SDL_SCANCODE_UNKNOWN && keys[s1]) return true;
  const SDL_Scancode s2 = sdl_scancode_for(b.secondary);
  if (s2 != SDL_SCANCODE_UNKNOWN && keys[s2]) return true;
  return false;
}

drive::PadControl pad_control_from_button(Uint8 b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return drive::PadControl::kButtonA;
    case SDL_GAMEPAD_BUTTON_EAST: return drive::PadControl::kButtonB;
    case SDL_GAMEPAD_BUTTON_WEST: return drive::PadControl::kButtonX;
    case SDL_GAMEPAD_BUTTON_NORTH: return drive::PadControl::kButtonY;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return drive::PadControl::kLeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return drive::PadControl::kRightShoulder;
    case SDL_GAMEPAD_BUTTON_BACK: return drive::PadControl::kBack;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return drive::PadControl::kDPadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return drive::PadControl::kDPadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return drive::PadControl::kDPadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return drive::PadControl::kDPadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return drive::PadControl::kLeftStickClick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return drive::PadControl::kRightStickClick;
    default: return drive::PadControl::kNone;
  }
}

double pad_control_value(SDL_Gamepad* pad, drive::PadControl c, double dz_in, double dz_out) {
  auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
  switch (c) {
    case drive::PadControl::kLeftTrigger:
      return drive::apply_deadzone_unsigned(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), dz_in, dz_out);
    case drive::PadControl::kRightTrigger:
      return drive::apply_deadzone_unsigned(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), dz_in, dz_out);
    case drive::PadControl::kButtonA: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ? 1.0 : 0.0;
    case drive::PadControl::kButtonB: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ? 1.0 : 0.0;
    case drive::PadControl::kButtonX: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST) ? 1.0 : 0.0;
    case drive::PadControl::kButtonY: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH) ? 1.0 : 0.0;
    case drive::PadControl::kLeftShoulder:
      return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 1.0 : 0.0;
    case drive::PadControl::kRightShoulder:
      return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 1.0 : 0.0;
    default: return 0.0;
  }
}

// Plumbs live AssistSettings into drive::Vehicle and race::DifficultyAssists.
void apply_vehicle_assists(drive::Vehicle& vehicle, const drive::AssistSettings& assists) {
  drive::VehicleAssists va;
  va.braking = assists.braking;
  va.steering = assists.steering;
  va.traction_control = assists.traction_control;
  va.stability_control = assists.stability_control;
  va.shifting = assists.shifting;
  vehicle.set_assists(va);
}

race::DifficultyAssists to_difficulty_assists(const drive::AssistSettings& a) {
  race::DifficultyAssists da;
  da.rewind_on = a.rewind;
  da.braking = a.braking;
  da.steering = a.steering;
  da.tcs_on = a.traction_control;
  da.stm_on = a.stability_control;
  da.shifting = a.shifting;
  da.driving_line = a.driving_line;
  return da;
}

// SPEC-CAMERAS' rig for the drive: the tuning from camera.zip (v4's layers
// when installed), the car's CameraOverrides row and MAXData box from the
// player's files, the CoM and steering wheel from the car in use. False
// (with a note) when something is missing: the drive keeps the old chase.
bool load_camera_rig(const std::filesystem::path& game_root, const std::filesystem::path& bake_dir,
                     const cars::bake::CarInfo& info, const drive::Vehicle& v, drive::DriveCamera& rig,
                     std::string& note, bool camera_effects, bool has_cockpit) {
  cars::Install install;
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  std::string ini, xml;
  drive::CameraTuning tuning;
  if (!cars::discover_install(game_root, state ? state : "", "", install, &note) ||
      !cars::load_camera_files(install, ini, xml, &note, camera_effects) ||
      !drive::parse_camera_settings(ini, tuning, &note) || !drive::parse_camera_physics(xml, tuning, &note))
    return false;
  // The box's z shift and the pristine box (the fallback) from the bake's rows
  // or directly from the disc in zero-bake mode.
  cars::CarPhysics physics;
  cars::PhysicsSettings settings_unused;
  {
    bool have_physics = false;
    if (!bake_dir.empty()) {
      std::string name = lower_ascii(info.media_name);
      std::ifstream in(cars::mods::resolve(bake_dir / "cars" / (name + ".physics.txt")), std::ios::binary);
      if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string e;
        have_physics = cars::physics_from_text(ss.str(), physics, settings_unused, &e);
      }
    }
    if (!have_physics) {
      std::string e;
      have_physics = drive::load_disc_car_physics(info.car_id, info.media_name, physics, settings_unused, &e);
    }
    if (!have_physics) {
      note = "no physics rows for " + info.media_name;
      return false;
    }
  }
  const double shift = physics.get("Data_CarBody.BottomCenterWheelbasePosZ");
  drive::CameraCar car;
  cars::CarCameraRow row;
  std::string row_error;
  if (cars::load_car_camera(install, info.car_id, info.media_name, row, &row_error) && row.has_box) {
    car.box_min = {row.box[0], row.box[1], row.box[2] - shift};
    car.box_max = {row.box[3], row.box[4], row.box[5] - shift};
  } else {
    // SPEC-CAMERAS, Unknowns: failing MAXData, the pristine box.
    note = row_error + "; the pristine box instead";
    const char* k = "Data_CarBody.PristineBoundingBox";
    car.box_min = {physics.get(std::string(k) + "MinX"), physics.get(std::string(k) + "MinY"),
                   physics.get(std::string(k) + "MinZ") - shift};
    car.box_max = {physics.get(std::string(k) + "MaxX"), physics.get(std::string(k) + "MaxY"),
                   physics.get(std::string(k) + "MaxZ") - shift};
  }
  car.overrides = row.overrides;
  car.has_cockpit_mesh = has_cockpit;
  // The CoM: the body frame (ground-origin, FORMAT-CARS' pose) to the car
  // frame (the model origin, z forward).
  const drive::Vec3 b = v.params().centre_of_mass;
  const double c = std::cos(info.pitch), s = std::sin(info.pitch), y = b.y - info.lift;
  car.com = {b.x, c * y + s * b.z, -s * y + c * b.z};
  for (int k = 0; k < 3; ++k) car.steering_wheel[size_t(k)] = info.part_positions[9][size_t(k)];
  rig.set_tuning(tuning);
  rig.set_car(car);
  return true;
}

ShowroomFrame camera_view_frame(const drive::CameraView& view, const cars::bake::CarInfo& info,
                                const drive::Vehicle& v, float aspect, bool reverse_z,
                                CameraProjectionState* cam_out) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(info, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  const V3 eye{float(view.eye[0]), float(view.eye[1]), float(view.eye[2])};
  const V3 at{float(view.eye[0] + view.forward[0]), float(view.eye[1] + view.forward[1]),
              float(view.eye[2] + view.forward[2])};
  const V3 up{float(view.up[0]), float(view.up[1]), float(view.up[2])};
  return look_frame(transform_v3(world_to_model, eye, 1), transform_v3(world_to_model, at, 1),
                    norm(transform_v3(world_to_model, up, 0)), aspect, false, reverse_z, float(view.fov_degrees),
                    float(view.near_clip), cam_out);
}

void apply_ndc_jitter(ShowroomFrame& f, float jx, float jy) {
  for (int c = 0; c < 4; ++c) {
    f.view_projection[c * 4 + 0] += jx * f.view_projection[c * 4 + 3];
    f.view_projection[c * 4 + 1] += jy * f.view_projection[c * 4 + 3];
    f.transparent_view_projection[c * 4 + 0] += jx * f.transparent_view_projection[c * 4 + 3];
    f.transparent_view_projection[c * 4 + 1] += jy * f.transparent_view_projection[c * 4 + 3];
  }
}

void compute_clip_from_world(const ShowroomFrame& f, const cars::bake::CarInfo& info, const drive::Vehicle& v,
                             float out[16]) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(info, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += f.view_projection[k * 4 + r] * world_to_model[c * 4 + k];
      out[c * 4 + r] = sum;
    }
  }
}

// The rig's frame: the car's pose and motion in, the view out (in the
// car's model frame, where the showroom draws).
ShowroomFrame rig_frame(drive::DriveCamera& rig, const cars::bake::CarInfo& info, const drive::Vehicle& v,
                        const drive::Ground* ground, carview::RigAccelState& accel, double dt, float aspect,
                        bool reverse_z, bool* interior, double* com_distance, drive::CameraView* out_view,
                        const world::Collision* collision, double car_coll_long, double car_coll_lat,
                        std::array<double, 4>* prev_wheel_comp, CameraProjectionState* cam_out) {
  float model_to_world[16], world_to_model[16];
  drive::model_to_world(info, v, model_to_world);
  invert_affine(model_to_world, world_to_model);
  drive::CameraInput in;
  // The car frame is the model frame with z negated.
  for (int k = 0; k < 16; ++k) in.car_to_world[k] = model_to_world[k] * (k >= 8 && k < 12 ? -1.0 : 1.0);
  const drive::Vec3 vel = v.velocity();
  in.speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
  const drive::Vec3 fwd = drive::rotate(v.orientation(), {0, 0, 1});
  in.heading = std::atan2(fwd.x, fwd.z);
  in.yaw_rate = v.angular_velocity().y;
  in.pitch_degrees = std::asin(std::clamp(fwd.y, -1.0, 1.0)) * 180 / 3.14159265358979323846;
  // The g layers' inputs: the body's acceleration over the frame along its
  // forward and right axes, gravity left out (SPEC-CAMERAS, Inputs).
  if (dt > 0.0) {
    accel.sample(vel, v.orientation(), dt, in.g_longitudinal, in.g_lateral);
  }
  int down = 0;
  const double c = std::cos(info.pitch), s = std::sin(info.pitch);
  const drive::Vec3 vpos = v.position();
  double slip_long_sum = 0, slip_lat_sum = 0;
  for (int i = 0; i < 4; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    down += w.contact ? 1 : 0;
    const double y = w.centre.y - v.params().tyre_radius[size_t(i)] - info.lift;
    in.wheel_ground_y[size_t(i)] = c * y + s * w.centre.z;
    // Surface bumpiness from procedural SurfaceType bump height + high-pass
    // suspension delta (prevents steady cornering lean from leaking into shake).
    double bump = 0.0;
    if (w.contact) {
      if (collision && w.surface >= 0 && size_t(w.surface) < collision->surfaces().size()) {
        const auto& sf = collision->surfaces()[size_t(w.surface)];
        drive::SurfaceTyre st{};
        st.bump_noise_type = sf.bump_noise_type;
        st.bump_frequency = sf.bump_frequency;
        st.bump_clamp_min = sf.bump_clamp_min;
        st.bump_clamp_max = sf.bump_clamp_max;
        st.bump_scale = sf.bump_scale;
        st.bump_offset = sf.bump_offset;
        const drive::Vec3 wp = vpos + drive::rotate(v.orientation(), w.centre);
        bump = drive::eval_surface_bumpiness(st, wp.x, wp.z).height;
      }
      if (prev_wheel_comp) {
        bump += (w.compression - (*prev_wheel_comp)[size_t(i)]);
        if (dt > 0.0) (*prev_wheel_comp)[size_t(i)] = w.compression;
      }
    } else if (prev_wheel_comp && dt > 0.0) {
      (*prev_wheel_comp)[size_t(i)] = 0.0;
    }
    in.surface_bump[size_t(i)] = bump;
    slip_long_sum += w.slip_ratio;
    slip_lat_sum += w.slip_angle;
  }
  in.contact = down / 4.0;
  in.slip_longitudinal = slip_long_sum * 0.25;
  in.slip_lateral = slip_lat_sum * 0.25;
  in.suspension_bias_longitudinal =
      0.5 * (v.wheels()[0].compression + v.wheels()[1].compression) -
      0.5 * (v.wheels()[2].compression + v.wheels()[3].compression);
  if (v.body_contact_force() > 0.0) {
    const auto& bc = v.strongest_body_contact();
    const double mag = bc.force / std::max(1.0, v.params().mass);
    const drive::Vec3 right = drive::rotate(v.orientation(), {1, 0, 0});
    in.world_collision_longitudinal = mag * (bc.normal.x * fwd.x + bc.normal.y * fwd.y + bc.normal.z * fwd.z);
    in.world_collision_lateral = mag * (bc.normal.x * right.x + bc.normal.y * right.y + bc.normal.z * right.z);
  }
  in.car_collision_longitudinal = car_coll_long;
  in.car_collision_lateral = car_coll_lat;
  if (ground) {
    in.ground_below = [ground](double x, double y, double z, double& gy) {
      drive::Hit hit;
      if (!ground->raycast({x, y, z}, {0, -1, 0}, 60, hit)) return false;
      gy = y - hit.distance;
      return true;
    };
    in.obstacle_ray = [ground](const std::array<double, 3>& origin, const std::array<double, 3>& dir, double max_dist,
                               double& hit_dist) {
      drive::Hit hit;
      if (!ground->raycast({origin[0], origin[1], origin[2]}, {dir[0], dir[1], dir[2]}, max_dist, hit)) return false;
      hit_dist = hit.distance;
      return true;
    };
  }
  const drive::CameraView view = rig.update(dt, in);
  if (out_view) *out_view = view;
  if (interior) *interior = view.interior;
  if (com_distance) {
    const drive::Vec3 com = v.centre_of_mass();
    *com_distance = std::hypot(view.eye[0] - com.x, view.eye[1] - com.y, view.eye[2] - com.z);
  }
  const V3 eye{float(view.eye[0]), float(view.eye[1]), float(view.eye[2])};
  const V3 at{float(view.eye[0] + view.forward[0]), float(view.eye[1] + view.forward[1]),
              float(view.eye[2] + view.forward[2])};
  const V3 up{float(view.up[0]), float(view.up[1]), float(view.up[2])};
  return look_frame(transform_v3(world_to_model, eye, 1), transform_v3(world_to_model, at, 1),
                    norm(transform_v3(world_to_model, up, 0)), aspect, false, reverse_z, float(view.fov_degrees),
                    float(view.near_clip), cam_out);
}

// The brake lamps' glow (an interim: FORMAT-CARS leaves the lit state to
// the effect): on while a front wheel gets the brake or the ABS works;
// SHIFT_BRAKE_GLOW (0 to 1) holds it for a render.
float brake_glow(const drive::Vehicle& v) {
  static const char* held = std::getenv("SHIFT_BRAKE_GLOW");
  if (held) return float(std::atof(held));
  const auto& w = v.wheels();
  return w[0].brake > 0.02 || w[1].brake > 0.02 || v.abs_active() ? 1.0f : 0.0f;
}

void wheel_poses(const drive::Vehicle& v, WheelPose poses[4]) {
  for (int i = 0; i < 4; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    poses[i].centre[0] = float(w.centre.x);
    poses[i].centre[1] = float(w.centre.y);
    poses[i].centre[2] = float(w.centre.z);
    poses[i].steer = float(w.steer);
    poses[i].spin = float(w.spin);
    poses[i].omega = float(w.omega);
  }
}

const char* kDriveRuns[6][2] = {
    {"rest", "0 0 0 0\n"},
    {"launch", "0 0 1 0\n"},
    {"slalom", "0 0 0.6 0\n2 0.8 0.5 0\n3 -0.8 0.5 0\n4 0.8 0.5 0\n5 -0.8 0.5 0\n6 0 0 0.6\n"},
    {"circle", "0 0 0 0\n1 0.5 0.35 0\n"},
    {"drift", "0 0 1 0\n1.8 0.95 1 0\n2.6 0.85 1 0\n3.4 -0.7 1 0\n4.2 0.85 1 0\n"},
    {"cruise", "0 0 0 0\n"},  // input from world::RoadFollower (needs --world)
};

// Flat colours per surface class (ours; the textured world's materials
// without a texture, and --flat-world): road, terrain, water, other, prop.
const std::array<float, 3> kSurfaceColours[] = {
    {0.035f, 0.035f, 0.04f}, {0.11f, 0.13f, 0.05f}, {0.03f, 0.07f, 0.12f}, {0.30f, 0.28f, 0.25f}, {0.20f, 0.17f, 0.12f}};

// The streamed mesh as showroom scenery, relative to `origin` (x, 0, z).
WorldTriangles scenery(const world::Mesh& mesh, double origin_x, double origin_z) {
  WorldTriangles w;
  w.colours.assign(std::begin(kSurfaceColours), std::end(kSurfaceColours));
  w.positions.reserve(mesh.positions.size());
  for (const auto& p : mesh.positions)
    w.positions.push_back({float(p[0] - origin_x), p[1], float(p[2] - origin_z)});
  w.group.reserve(mesh.triangles());
  for (auto s : mesh.surface) w.group.push_back(uint8_t(s));
  if (mesh.has_render_data()) {
    w.normals = mesh.normals;
    w.uv0 = mesh.uv0;
    w.uv1 = mesh.uv1;
    w.uv2 = mesh.uv2;
    w.vertex_colours = mesh.colours;
    w.material = mesh.material;
    // Prop origins in the scenery's frame, like the positions.
    w.instance.reserve(mesh.instance.size());
    for (const auto& p : mesh.instance) w.instance.push_back({float(p[0] - origin_x), p[1], float(p[2] - origin_z)});
    for (const auto& m : mesh.materials) {
      WorldTriangles::Material wm;
      wm.texture = m.albedo;
      wm.blend = uint8_t(m.blend);
      wm.far = m.far;
      wm.decal = uint8_t(world::decal_effect(m.effect) ? 1 : 0);
      wm.layers = uint8_t(m.layers);
      wm.layer_b = m.layer_b;
      wm.splat = m.splat;
      wm.light_map = m.light_map;
      wm.normal_map = m.normal_map;
      wm.ao_map = m.ao_map;
      wm.layer_c = m.layer_c;
      wm.spec_map = m.spec_map;
      wm.tiling = m.tiling;
      wm.tiling_c = m.tiling_c;
      wm.decal_noise_c4 = m.decal_noise_c4;
      wm.emissive_force = m.emissive_force;
      wm.is_emissive_map = m.is_emissive_map;
      wm.is_water = m.is_water;
      wm.anim_flag = m.anim_flag;
      wm.is_grass = m.is_grass;
      wm.is_road = m.is_road;
      wm.fade = m.fade;
      w.materials.push_back(wm);
    }
  }
  return w;
}

// The world textures decoded so far, by texture record. Only the gathers'

}  // namespace carview
