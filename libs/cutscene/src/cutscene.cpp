#include "cutscene/cutscene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "audio/xml.h"
#include "fh1data/archive.h"

namespace cutscene {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

bool ieq(std::string_view a, std::string_view b) { return audio::iequals(a, b); }

AnimSpace parse_space(std::string_view s) {
  if (ieq(s, "CarSpace")) return AnimSpace::kCarSpace;
  if (ieq(s, "PartSpace")) return AnimSpace::kPartSpace;
  if (ieq(s, "TargetPart")) return AnimSpace::kTargetPart;
  return s.empty() ? AnimSpace::kCarSpace : AnimSpace::kUnknown;
}

CurveType parse_curve(std::string_view s) { return ieq(s, "S") ? CurveType::kS : CurveType::kLinear; }

FadeOptions parse_fade(const audio::XmlNode* n) {
  FadeOptions f;
  if (!n) return f;
  f.duration = n->number("Duration");
  f.hold = n->number("HoldDuration");
  f.curve = parse_curve(n->attr("CurveType"));
  f.color = {float(n->number("Color.x")), float(n->number("Color.y")), float(n->number("Color.z")),
             float(n->number("Color.w"))};
  return f;
}

bool flag(const audio::XmlNode& n, std::string_view key) { return n.number(key) != 0.0; }

Cam parse_cam(const audio::XmlNode& c) {
  Cam cam;
  cam.start = c.number("StartCut");
  cam.duration = c.number("duration");
  cam.target_name = c.attr("TargetName");
  cam.follow = ieq(c.attr("Type"), "Follow");
  cam.disable_csm = flag(c, "DisableCSM");
  cam.disable_cubemap = flag(c, "DisableCubemap");
  cam.disable_car_rendering = flag(c, "DisableCarRendering");
  cam.apply_motion_blur = flag(c, "ApplyMotionBlur");
  if (const auto* a = c.child("Anim")) {
    cam.pos_space = parse_space(a->attr("PosAnimType"));
    cam.rot_space = parse_space(a->attr("RotAnimType"));
    cam.rot_part_target = a->attr("RotAnimPartToTarget");
    cam.time_anim_type = a->attr("TimeAnimType");
    cam.in_cockpit = flag(*a, "IsInCockpit");
    cam.ease_in = a->number("EaseIn");
    cam.ease_out = a->number("EaseOut");
    cam.const_vel_speed = a->number("ConstVelCamSpeed");
    for (const auto& k : a->children) {
      if (!ieq(k.name, "Key")) continue;
      CamKey key;
      key.time = k.number("Time");
      key.pos = {k.number("PosX"), k.number("PosY"), k.number("PosZ")};
      key.yaw = k.number("Yaw");
      key.pitch = k.number("Pitch");
      key.roll = k.number("Roll");
      key.fov = k.number("FOV", 50.0);
      key.time_c0 = k.number("TimeC0");
      key.car_part = k.attr("CarPart");
      cam.keys.push_back(std::move(key));
    }
    std::stable_sort(cam.keys.begin(), cam.keys.end(),
                     [](const CamKey& x, const CamKey& y) { return x.time < y.time; });
  }
  if (const auto* p = c.child("PostEffectsAnim")) {
    const std::string mode = p->attr("DOFMode");
    cam.dof = ieq(mode, "Manual") ? DofMode::kManual : (ieq(mode, "Off") || mode.empty()) ? DofMode::kOff
                                                                                          : DofMode::kOther;
    cam.motion_blur = p->number("MotionBlurAmount");
    for (const auto& k : p->children) {
      if (!ieq(k.name, "Key")) continue;
      PostKey key;
      key.time = k.number("Time");
      key.focal_distance = k.number("FocalDistance");
      key.focal_length = k.number("FocalLength");
      key.focal_max_blur = k.number("FocalMaxBlur");
      key.saturation = k.number("Saturation");
      key.exposure = k.number("Exposure");
      key.brightness = k.number("Brightness");
      key.contrast = k.number("Contrast");
      key.sepia = k.number("Sepia");
      key.blur = k.number("Blur");
      cam.post_keys.push_back(key);
    }
    std::stable_sort(cam.post_keys.begin(), cam.post_keys.end(),
                     [](const PostKey& x, const PostKey& y) { return x.time < y.time; });
  }
  cam.fade_in = parse_fade(c.child("FadeInOptions"));
  cam.fade_out = parse_fade(c.child("FadeOutOptions"));
  if (const auto* ph = c.child("CameraPhysics")) cam.physics_layers = int(ph->number("NumLayers"));
  return cam;
}

double hermite(double p0, double m0, double p1, double m1, double s, double dt) {
  const double s2 = s * s, s3 = s2 * s;
  const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
  return h00 * p0 + h10 * dt * m0 + h01 * p1 + h11 * dt * m1;
}

// d(index)/d(time) at key k for the time-to-index map.
double index_tangent(const std::vector<CamKey>& keys, size_t k) {
  const size_t n = keys.size();
  auto secant = [&](size_t i) {  // between key i and i + 1
    const double dt = keys[i + 1].time - keys[i].time;
    return dt > 0 ? 1.0 / dt : 0.0;
  };
  if (n < 2) return 0.0;
  if (n == 2) return secant(0);
  auto interior = [&](size_t i) {
    const double dt = keys[i + 1].time - keys[i - 1].time;
    return dt > 0 ? 2.0 / dt : 0.0;
  };
  if (k == 0) return 0.5 * (3.0 * secant(0) - interior(1));
  if (k == n - 1) return 0.5 * (3.0 * secant(n - 2) - interior(n - 2));
  return interior(k);
}

std::array<double, 7> channels(const CamKey& k) {
  return {k.pos[0], k.pos[1], k.pos[2], k.yaw, k.pitch, k.roll, k.fov};
}

std::array<double, 3> rotate_heading(const std::array<double, 3>& v, double h) {
  const double c = std::cos(h), s = std::sin(h);
  return {v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c};
}

std::array<double, 3> normalise(std::array<double, 3> v) {
  const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 0) v = {v[0] / l, v[1] / l, v[2] / l};
  return v;
}

}  // namespace

EventType event_type_from_id(std::string_view id) {
  struct Entry {
    const char* id;
    EventType type;
  };
  static const Entry kTable[] = {
      {"CCutsceneCameraNodeTrigger", EventType::kCameraNode},
      {"CCutscenePlaceCarTrigger", EventType::kPlacePlayerCar},
      {"CCutsceneCarTrigger", EventType::kCar},
      {"CCutsceneReleaseCarTrigger", EventType::kReleaseCar},
      {"CCutsceneAudioTrigger", EventType::kAudio},
      {"CCutsceneVoiceOverTrigger", EventType::kVoiceOver},
      {"CCutsceneVoiceOverFestivalAnnouncerTrigger", EventType::kVoiceOver},
      {"CCutsceneAudioMixSnapshotTrigger", EventType::kMixSnapshot},
      {"CCutsceneMusicTrigger", EventType::kMusic},
      {"CCutsceneShowDriverTrigger", EventType::kShowDriver},
      {"CCutsceneAnimationTrigger", EventType::kAnimation},
      {"CCutsceneCountdownTrigger", EventType::kCountdown},
      {"CCutsceneStartCountdownTrigger", EventType::kStartCountdown},
      {"CCutsceneShowBarnFindHUDSceneTrigger", EventType::kBarnFindHud},
      {"CCutsceneNemesisUITrigger", EventType::kNemesisUi},
  };
  for (const auto& e : kTable)
    if (ieq(id, e.id)) return e.type;
  return EventType::kOther;
}

const char* event_type_name(EventType type) {
  switch (type) {
    case EventType::kCameraNode: return "camera_node";
    case EventType::kPlacePlayerCar: return "place_player_car";
    case EventType::kCar: return "car";
    case EventType::kReleaseCar: return "release_car";
    case EventType::kAudio: return "audio";
    case EventType::kVoiceOver: return "voice_over";
    case EventType::kMixSnapshot: return "mix_snapshot";
    case EventType::kMusic: return "music";
    case EventType::kShowDriver: return "show_driver";
    case EventType::kAnimation: return "animation";
    case EventType::kCountdown: return "countdown";
    case EventType::kStartCountdown: return "start_countdown";
    case EventType::kBarnFindHud: return "barn_find_hud";
    case EventType::kNemesisUi: return "nemesis_ui";
    case EventType::kOther: break;
  }
  return "other";
}

std::string Event::attr(std::string_view key, std::string fallback) const {
  for (const auto& [k, v] : attributes)
    if (ieq(k, key)) return v;
  return fallback;
}

double Event::number(std::string_view key, double fallback) const {
  for (const auto& [k, v] : attributes) {
    if (!ieq(k, key)) continue;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return end != v.c_str() ? d : fallback;
  }
  return fallback;
}

bool Event::has(std::string_view key) const {
  for (const auto& [k, v] : attributes)
    if (ieq(k, key)) return true;
  return false;
}

int Cutscene::cam_at(double t) const {
  int best = -1;
  for (size_t i = 0; i < cams.size(); ++i) {
    if (cams[i].start <= t + 1e-9) {
      if (best < 0 || cams[i].start >= cams[size_t(best)].start) best = int(i);
    }
  }
  if (best < 0 && !cams.empty()) best = 0;
  return best;
}

const Cutscene* Library::find(std::string_view name) const {
  for (const auto& c : cutscenes)
    if (ieq(c.name, name)) return &c;
  return nullptr;
}

namespace {

// A camera sequence's cams: a ResetPlayback cam marks the loop's end (the
// sequence's duration when it states none) and is dropped; a cam of zero
// duration runs to the next cam's start.
Cutscene camera_sequence(const audio::XmlNode& cams, std::string name, double duration) {
  Cutscene cs;
  cs.name = std::move(name);
  cs.mode = PlaybackMode::kLooping;
  cs.duration = duration;
  cs.declared_cams = int(cams.number("NumCams"));
  double reset = -1.0;
  for (const auto& cam : cams.children) {
    if (!ieq(cam.name, "Cam")) continue;
    if (ieq(cam.attr("Type"), "ResetPlayback")) {
      reset = cam.number("StartCut");
      continue;
    }
    cs.cams.push_back(parse_cam(cam));
  }
  std::stable_sort(cs.cams.begin(), cs.cams.end(), [](const Cam& a, const Cam& b) { return a.start < b.start; });
  if (cs.duration <= 0.0) cs.duration = reset > 0.0 ? reset : (cs.cams.empty() ? 0.0 : cs.cams.back().end());
  for (size_t i = 0; i < cs.cams.size(); ++i)
    if (cs.cams[i].duration <= 0.0)
      cs.cams[i].duration = std::max(0.0, (i + 1 < cs.cams.size() ? cs.cams[i + 1].start : cs.duration) - cs.cams[i].start);
  return cs;
}

}  // namespace

bool parse_camera_sequences(std::string_view xml, Library& out, std::string* error) {
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  out.cutscenes.clear();
  if (ieq(root.name, "Cams")) {
    out.cutscenes.push_back(camera_sequence(root, "", 0.0));
    return true;
  }
  for (const auto& c : root.children) {
    if (!ieq(c.name, "Cutscene")) continue;
    if (const auto* cams = c.child("Cams")) out.cutscenes.push_back(camera_sequence(*cams, c.attr("name"), c.number("Duration")));
  }
  if (out.cutscenes.empty()) {
    if (error) *error = "root element " + root.name + " holds no camera sequence";
    return false;
  }
  return true;
}

bool parse_cutscenes(std::string_view xml, Library& out, std::string* error) {
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  if (!ieq(root.name, "CutsceneManager")) {
    if (error) *error = "root element is " + root.name + ", not CutsceneManager";
    return false;
  }
  out.cutscenes.clear();
  for (const auto& c : root.children) {
    if (!ieq(c.name, "Cutscene")) continue;
    Cutscene cs;
    cs.name = c.attr("name");
    cs.mode = ieq(c.attr("PlaybackMode"), "Looping") ? PlaybackMode::kLooping : PlaybackMode::kNormal;
    cs.duration = c.number("Duration");
    cs.loop_from = c.number("LoopFrom");
    cs.mirror_for_rhd = flag(c, "MirrorForRightHandDrive");
    if (const auto* cams = c.child("Cams")) {
      cs.declared_cams = int(cams->number("NumCams"));
      for (const auto& cam : cams->children)
        if (ieq(cam.name, "Cam")) cs.cams.push_back(parse_cam(cam));
    }
    if (const auto* events = c.child("Events")) {
      for (const auto& e : events->children) {
        if (!ieq(e.name, "Event")) continue;
        const double t = e.number("time");
        for (const auto& tr : e.children) {
          if (!ieq(tr.name, "EventTrigger")) continue;
          Event ev;
          ev.time = t;
          ev.id = tr.attr("id");
          ev.name = tr.attr("name");
          ev.type = event_type_from_id(ev.id);
          ev.attributes = tr.attributes;
          cs.events.push_back(std::move(ev));
        }
      }
      std::stable_sort(cs.events.begin(), cs.events.end(),
                       [](const Event& a, const Event& b) { return a.time < b.time; });
    }
    out.cutscenes.push_back(std::move(cs));
  }
  return true;
}

double key_parameter(const std::vector<CamKey>& keys, double t) {
  const size_t n = keys.size();
  if (n < 2) return 0.0;
  if (t <= keys.front().time) return 0.0;
  if (t >= keys.back().time) return double(n - 1);
  size_t i = 0;
  while (i + 2 < n && keys[i + 1].time <= t) ++i;
  const double t0 = keys[i].time, t1 = keys[i + 1].time, dt = t1 - t0;
  if (dt <= 0) return double(i) + 0.5;
  const double s = (t - t0) / dt;
  const double u = hermite(double(i), index_tangent(keys, i), double(i + 1), index_tangent(keys, i + 1), s, dt);
  return std::clamp(u, double(i), double(i + 1));
}

Pose pose_at_parameter(const std::vector<CamKey>& keys, double u) {
  Pose p;
  const size_t n = keys.size();
  if (n == 0) return p;
  auto set = [&](const std::array<double, 7>& c) {
    p.pos = {c[0], c[1], c[2]};
    p.yaw = c[3], p.pitch = c[4], p.roll = c[5], p.fov = c[6];
  };
  if (n == 1) {
    set(channels(keys[0]));
    return p;
  }
  u = std::clamp(u, 0.0, double(n - 1));
  size_t i = size_t(std::floor(u));
  if (i > n - 2) i = n - 2;
  const double f = u - double(i);
  auto unwrap_near = [](double val, double ref) {
    while (val - ref > 180.0) val -= 360.0;
    while (val - ref < -180.0) val += 360.0;
    return val;
  };
  const auto c0 = channels(keys[i]);
  auto c1 = channels(keys[i + 1]);
  auto c_prev = i > 0 ? channels(keys[i - 1]) : c0;
  auto c_next = (i + 1 < n - 1) ? channels(keys[i + 2]) : c1;
  // Unwrap Euler angle channels (yaw=3, pitch=4, roll=5) across +-180 deg
  // boundaries within the Catmull-Rom stencil so cuts like 361.99 -> -0.23
  // pan smoothly instead of spinning 360 deg.
  for (size_t ch = 3; ch <= 5; ++ch) {
    if (i > 0) c_prev[ch] = unwrap_near(c_prev[ch], c0[ch]);
    c1[ch] = unwrap_near(c1[ch], c0[ch]);
    if (i + 1 < n - 1) c_next[ch] = unwrap_near(c_next[ch], c1[ch]);
  }
  std::array<double, 7> m0{}, m1{}, out{};
  for (size_t ch = 0; ch < 7; ++ch) {
    m0[ch] = (i == 0) ? c1[ch] - c0[ch] : 0.5 * (c1[ch] - c_prev[ch]);
    m1[ch] = (i + 1 == n - 1) ? c1[ch] - c0[ch] : 0.5 * (c_next[ch] - c0[ch]);
    out[ch] = hermite(c0[ch], m0[ch], c1[ch], m1[ch], f, 1.0);
  }
  set(out);
  return p;
}

Pose sample_pose(const Cam& cam, double local_t) { return pose_at_parameter(cam.keys, key_parameter(cam.keys, local_t)); }

PostSample sample_post(const Cam& cam, double local_t) {
  PostSample s;
  s.dof = cam.dof == DofMode::kManual;
  s.motion_blur = cam.motion_blur;
  const auto& k = cam.post_keys;
  if (k.empty()) return s;
  auto assign = [&](const PostKey& a, const PostKey& b, double f) {
    auto mix = [f](double x, double y) { return x + (y - x) * f; };
    s.focal_distance = mix(a.focal_distance, b.focal_distance);
    s.focal_length = mix(a.focal_length, b.focal_length);
    s.focal_max_blur = mix(a.focal_max_blur, b.focal_max_blur);
    s.saturation = mix(a.saturation, b.saturation);
    s.exposure = mix(a.exposure, b.exposure);
    s.brightness = mix(a.brightness, b.brightness);
    s.contrast = mix(a.contrast, b.contrast);
    s.sepia = mix(a.sepia, b.sepia);
    s.blur = mix(a.blur, b.blur);
  };
  if (local_t <= k.front().time) {
    assign(k.front(), k.front(), 0.0);
  } else if (local_t >= k.back().time) {
    assign(k.back(), k.back(), 0.0);
  } else {
    size_t i = 0;
    while (i + 2 < k.size() && k[i + 1].time <= local_t) ++i;
    const double dt = k[i + 1].time - k[i].time;
    assign(k[i], k[i + 1], dt > 0 ? (local_t - k[i].time) / dt : 1.0);
  }
  return s;
}

double fade_curve(CurveType curve, double x) {
  x = std::clamp(x, 0.0, 1.0);
  return curve == CurveType::kS ? x * x * (3.0 - 2.0 * x) : x;
}

Overlay fade_overlay(const FadeOptions& fade, double t, bool fade_in) {
  Overlay o;
  o.rgb = {fade.color[0], fade.color[1], fade.color[2]};
  if (!fade.active()) return o;
  double cover;
  if (t < fade.hold) {
    cover = 1.0;  // the colour is held first
  } else {
    const double x = fade.duration > 0 ? (t - fade.hold) / fade.duration : 1.0;
    const double c = fade_curve(fade.curve, x);
    cover = fade_in ? 1.0 - c : c;
  }
  if (!fade_in && t < fade.hold) cover = 0.0;
  o.alpha = float(std::clamp(cover, 0.0, 1.0));
  return o;
}

Overlay cutscene_overlay(const Cutscene& cs, double t) {
  Overlay best;
  for (const auto& cam : cs.cams) {
    if (t < cam.start - 1e-9 || t > cam.end() + 1e-9) continue;
    const double local = t - cam.start;
    if (cam.fade_in.active() && local <= cam.fade_in.hold + cam.fade_in.duration) {
      const Overlay o = fade_overlay(cam.fade_in, local, true);
      if (o.alpha > best.alpha) best = o;
    }
    if (cam.fade_out.active()) {
      const double from = cam.duration - cam.fade_out.duration;
      if (local >= from) {
        FadeOptions f = cam.fade_out;
        f.hold = 0.0;
        const Overlay o = fade_overlay(f, local - from, false);
        if (o.alpha > best.alpha) best = o;
      }
    }
  }
  return best;
}

Sample sample(const Cutscene& cs, double t) {
  Sample s;
  s.cam_index = cs.cam_at(t);
  if (s.cam_index >= 0) {
    const Cam& cam = cs.cams[size_t(s.cam_index)];
    s.local_time = std::clamp(t - cam.start, 0.0, std::max(0.0, cam.duration));
    s.pose = sample_pose(cam, s.local_time);
    s.post = sample_post(cam, s.local_time);
  }
  s.overlay = cutscene_overlay(cs, t);
  return s;
}

WorldCamera to_world(const Pose& pose, const Frame& frame, bool in_cockpit) {
  WorldCamera w;
  const double h = frame.heading_deg * kDeg;
  const auto off = rotate_heading(pose.pos, h);
  w.eye = {frame.origin[0] + off[0], frame.origin[1] + off[1], frame.origin[2] + off[2]};
  const double yaw = h + pose.yaw * kDeg, pitch = pose.pitch * kDeg, roll = pose.roll * kDeg;
  // Pitch is positive down (the driving cameras' convention).
  const std::array<double, 3> f = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
                                   std::cos(yaw) * std::cos(pitch)};
  // Right of the un-rolled camera, then the up vector, rolled about forward.
  const std::array<double, 3> right = normalise({std::cos(yaw), 0.0, -std::sin(yaw)});
  const std::array<double, 3> up0 = {f[1] * right[2] - f[2] * right[1], f[2] * right[0] - f[0] * right[2],
                                     f[0] * right[1] - f[1] * right[0]};
  // up0 = forward x right (the frame is left-handed with y up). A positive
  // roll turns the camera counter-clockwise seen from behind: its top leans left.
  const double c = std::cos(roll), s = std::sin(roll);
  w.forward = normalise(f);
  w.up = normalise({up0[0] * c - right[0] * s, up0[1] * c - right[1] * s, up0[2] * c - right[2] * s});
  w.fov_deg = pose.fov;
  w.interior = in_cockpit;
  return w;
}

void Player::start(const Cutscene* cs) {
  cs_ = cs;
  t_ = 0.0;
  started_ = false;
  finished_ = cs == nullptr;
  hold_ = false;
  exit_vo_ = ExitVo::kNone;
  exit_vo_wait_ = 0.0;
  nodes_.clear();
  node_meta_.clear();
  car_frame_override_.reset();
  fired_.assign(cs ? cs->events.size() : 0, false);
}

void Player::stop() {
  cs_ = nullptr;
  finished_ = true;
  nodes_.clear();
  node_meta_.clear();
  car_frame_override_.reset();
  fired_.clear();
}

Frame Player::snap_frame(const Frame& f, bool snap_to_ground) const {
  if (!snap_to_ground || !ground_query_) return f;
  double gy = f.origin[1];
  if (ground_query_(f.origin[0], f.origin[1], f.origin[2], gy)) {
    Frame snapped = f;
    snapped.origin[1] = gy;
    return snapped;
  }
  return f;
}

void Player::fire_range(double t0, double t1, bool inclusive_start, std::vector<const Event*>& out) {
  for (size_t i = 0; i < cs_->events.size(); ++i) {
    if (fired_[i]) continue;
    const double et = cs_->events[i].time;
    const bool in = inclusive_start ? (et >= t0 - 1e-9 && et <= t1 + 1e-9) : (et > t0 && et <= t1 + 1e-9);
    if (!in) continue;
    fired_[i] = true;
    const Event& ev = cs_->events[i];
    if (ev.type == EventType::kCameraNode) {
      const bool snap = ev.number("snapToGround") != 0.0;
      // Store the un-snapped composed world frame and source event so both
      // root and parented snapToGround=1 nodes lazily re-snap once terrain
      // streams in.
      const Frame raw = event_frame(ev, 0, /*apply_snap=*/false);
      const Frame f = snap_frame(raw, snap);
      bool replaced = false;
      for (size_t ni = 0; ni < nodes_.size(); ++ni) {
        if (ieq(nodes_[ni].first, ev.name)) {
          nodes_[ni].second = f;
          if (ni < node_meta_.size()) node_meta_[ni] = {ev.name, raw, snap, &ev};
          replaced = true;
        }
      }
      if (!replaced) {
        nodes_.emplace_back(ev.name, f);
        node_meta_.push_back({ev.name, raw, snap, &ev});
      }
    }
    if (ev.type == EventType::kVoiceOver && ev.number("exitCutsceneOnFinish") == 1.0) {
      exit_vo_ = ExitVo::kFired;
      exit_vo_wait_ = 0.0;
    }
    out.push_back(&ev);
  }
}

Frame Player::event_frame(const Event& e) const { return event_frame(e, 0, true); }

Frame Player::event_frame(const Event& e, int depth, bool apply_snap) const {
  const Frame local{e.position(), e.rotation_deg()};
  const bool snap = apply_snap && (e.number("snapToGround") != 0.0);
  const std::string parent = e.attr("parent");
  if (parent.empty() || depth > 4 || !cs_) return snap_frame(local, snap);
  // The parent: a node already fired under that name, else the cutscene's
  // placing trigger of that name (camera node, placed car, spawned car, or
  // animation trigger), else a scene node.
  Frame pf{};
  bool found = false;
  for (size_t ni = 0; ni < nodes_.size(); ++ni) {
    if (ieq(nodes_[ni].first, parent)) {
      if (ni < node_meta_.size() && node_meta_[ni].event && node_meta_[ni].event != &e) {
        pf = event_frame(*node_meta_[ni].event, depth + 1, true);
      } else if (ni < node_meta_.size() && node_meta_[ni].snap_to_ground) {
        pf = snap_frame(node_meta_[ni].raw, true);
      } else {
        pf = nodes_[ni].second;
      }
      found = true;
      break;
    }
  }
  if (!found) {
    for (const auto& p : cs_->events) {
      if (&p != &e && ieq(p.name, parent) &&
          (p.type == EventType::kCameraNode || p.type == EventType::kPlacePlayerCar ||
           p.type == EventType::kCar || p.type == EventType::kAnimation)) {
        pf = event_frame(p, depth + 1, true);
        found = true;
        break;
      }
    }
  }
  if (!found) {
    for (const auto& [n, f] : scene_nodes_) {
      if (ieq(n, parent)) {
        pf = f;
        found = true;
        break;
      }
    }
  }
  if (!found) return snap_frame(local, snap);
  const auto off = rotate_heading(local.origin, pf.heading_deg * kDeg);
  const Frame composed{{pf.origin[0] + off[0], pf.origin[1] + off[1], pf.origin[2] + off[2]},
                       pf.heading_deg + local.heading_deg};
  return snap_frame(composed, snap);
}

void Player::set_voice_playing(bool playing) {
  if (exit_vo_ == ExitVo::kFired && playing) exit_vo_ = ExitVo::kPlaying;
  else if (exit_vo_ == ExitVo::kPlaying && !playing) exit_vo_ = ExitVo::kDone;
}

bool Player::holding() const {
  return hold_ && (exit_vo_ == ExitVo::kPlaying || (exit_vo_ == ExitVo::kFired && exit_vo_wait_ < kVoiceStartGrace));
}

std::vector<const Event*> Player::step(double dt) {
  std::vector<const Event*> out;
  if (!cs_ || finished_) return out;
  if (hold_ && exit_vo_ == ExitVo::kDone) {
    // exitCutsceneOnFinish: the cutscene ends with its voice-over.
    finished_ = true;
    return out;
  }
  if (exit_vo_ == ExitVo::kFired) exit_vo_wait_ += std::max(0.0, dt);
  if (!started_) {
    started_ = true;
    fire_range(0.0, 0.0, true, out);
  }
  if (dt <= 0) return out;
  const double t0 = t_;
  double t1 = t_ + dt;
  const double end = cs_->duration;
  const bool loops = cs_->mode == PlaybackMode::kLooping || (holding() && cs_->loop_from > 0.0);
  if (t1 >= end && end > 0) {
    fire_range(t0, end, false, out);
    if (loops && cs_->loop_from < end) {
      const double span = end - cs_->loop_from;
      t1 = cs_->loop_from + std::fmod(t1 - end, span);
    } else {
      t_ = end;
      finished_ = true;
      return out;
    }
  } else {
    fire_range(t0, t1, false, out);
  }
  t_ = t1;
  return out;
}

Sample Player::current() const {
  if (!cs_) return {};
  return sample(*cs_, t_);
}

bool find_part(const CarParts& parts, std::string_view name, std::array<double, 3>& out) {
  for (const auto& p : parts)
    if (ieq(p.name, name)) {
      out = p.pos;
      return true;
    }
  return false;
}

Frame part_frame(const Frame& car, const std::array<double, 3>& part_pos) {
  const auto off = rotate_heading(part_pos, car.heading_deg * kDeg);
  return {{car.origin[0] + off[0], car.origin[1] + off[1], car.origin[2] + off[2]}, car.heading_deg};
}

Frame Player::frame_for(const Cam& cam, const Frame& player_car, const CarParts* parts, double local_t) const {
  if (!cam.target_name.empty()) {
    for (size_t ni = 0; ni < nodes_.size(); ++ni) {
      if (ieq(nodes_[ni].first, cam.target_name)) {
        if (ni < node_meta_.size() && node_meta_[ni].event) return event_frame(*node_meta_[ni].event, 0, true);
        if (ni < node_meta_.size() && node_meta_[ni].snap_to_ground) return snap_frame(node_meta_[ni].raw, true);
        return nodes_[ni].second;
      }
    }
    // Not fired yet (a seek, or a cut that starts with its node): the
    // cutscene's placing trigger of that name.
    if (cs_) {
      for (const auto& e : cs_->events)
        if (e.type == EventType::kCameraNode && ieq(e.name, cam.target_name)) return event_frame(e);
      for (const auto& e : cs_->events)
        if (e.type == EventType::kAnimation && ieq(e.name, cam.target_name)) return event_frame(e);
      for (const auto& e : cs_->events)
        if ((e.type == EventType::kCar || e.type == EventType::kPlacePlayerCar) && ieq(e.name, cam.target_name))
          return event_frame(e);
    }
    for (const auto& [n, f] : scene_nodes_)
      if (ieq(n, cam.target_name)) return f;
  }
  const Frame car_base = car_frame_override_.value_or(player_car);
  if ((cam.pos_space == AnimSpace::kPartSpace || cam.pos_space == AnimSpace::kTargetPart) && parts) {
    std::array<double, 3> p{};
    if (cam.pos_space == AnimSpace::kTargetPart && !cam.rot_part_target.empty() &&
        find_part(*parts, cam.rot_part_target, p)) {
      return part_frame(car_base, p);
    }
    if (!cam.keys.empty()) {
      const double u = key_parameter(cam.keys, local_t);
      const size_t k = std::min(cam.keys.size() - 1, size_t(std::max(0.0, std::floor(u + 0.5))));
      if (find_part(*parts, cam.keys[k].car_part, p) || find_part(*parts, cam.keys[0].car_part, p))
        return part_frame(car_base, p);
    }
  }
  // CarSpace cams targeting the car are authored relative to the car's body
  // origin (`car.lift` above the road contact point), matching model_to_world.
  Frame car_space = car_base;
  car_space.origin[1] += car_body_offset_y_;
  return car_space;
}

bool Player::world_camera(const Frame& player_car, WorldCamera& out, const CarParts* parts) const {
  if (!cs_ || cs_->cams.empty()) return false;
  const Sample s = current();
  if (s.cam_index < 0) return false;
  const Cam& cam = cs_->cams[size_t(s.cam_index)];
  if (cam.follow) return false;  // the game's own camera
  const Frame frame = frame_for(cam, player_car, parts, s.local_time);
  out = to_world(s.pose, frame, cam.in_cockpit);
  if (cam.rot_space == AnimSpace::kTargetPart && parts) {
    std::string_view part_name = cam.rot_part_target;
    if (part_name.empty() && !cam.keys.empty()) {
      const double u = key_parameter(cam.keys, s.local_time);
      const size_t k = std::min(cam.keys.size() - 1, size_t(std::max(0.0, std::floor(u + 0.5))));
      part_name = !cam.keys[k].car_part.empty() ? std::string_view(cam.keys[k].car_part)
                                                : std::string_view(cam.keys[0].car_part);
    }
    std::array<double, 3> part_local{};
    if (!part_name.empty() && find_part(*parts, part_name, part_local)) {
      const Frame car_base = car_frame_override_.value_or(player_car);
      const Frame target = part_frame(car_base, part_local);
      const std::array<double, 3> dir{target.origin[0] - out.eye[0], target.origin[1] - out.eye[1],
                                      target.origin[2] - out.eye[2]};
      const double dist = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      if (dist > 1e-4) {
        const double base_yaw = std::atan2(dir[0], dir[2]);
        const double base_pitch = -std::asin(std::clamp(dir[1] / dist, -1.0, 1.0));
        const double yaw = base_yaw + s.pose.yaw * kDeg;
        const double pitch = std::clamp(base_pitch + s.pose.pitch * kDeg, -1.55, 1.55);
        const double roll = s.pose.roll * kDeg;
        const std::array<double, 3> f{std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
                                      std::cos(yaw) * std::cos(pitch)};
        const std::array<double, 3> right = normalise({std::cos(yaw), 0.0, -std::sin(yaw)});
        const std::array<double, 3> up0{f[1] * right[2] - f[2] * right[1], f[2] * right[0] - f[0] * right[2],
                                        f[0] * right[1] - f[1] * right[0]};
        const double c = std::cos(roll), sn = std::sin(roll);
        out.forward = normalise(f);
        out.up = normalise({up0[0] * c - right[0] * sn, up0[1] * c - right[1] * sn, up0[2] * c - right[2] * sn});
      }
    }
  }
  if (!out.interior && eye_clearance_query_) {
    double gy = 0.0;
    if (eye_clearance_query_(out.eye[0], out.eye[1], out.eye[2], gy)) {
      const double min_y = gy + kMinEyeGroundClearance;
      if (out.eye[1] < min_y) {
        // Re-aim gently toward the original focus point along `out.forward` so
        // lifting eye[1] out of the ground preserves framing instead of burying
        // the subject off the bottom of the screen.
        const std::array<double, 3> orig_eye = out.eye;
        const std::array<double, 3> to_origin{frame.origin[0] - orig_eye[0], frame.origin[1] - orig_eye[1],
                                              frame.origin[2] - orig_eye[2]};
        const double proj = to_origin[0] * out.forward[0] + to_origin[1] * out.forward[1] +
                            to_origin[2] * out.forward[2];
        const double origin_dist =
            std::sqrt(to_origin[0] * to_origin[0] + to_origin[1] * to_origin[1] + to_origin[2] * to_origin[2]);
        const double focus_dist = proj > 0.5 ? std::clamp(proj, 1.5, 40.0) : std::clamp(origin_dist, 3.0, 25.0);
        const std::array<double, 3> aim{orig_eye[0] + out.forward[0] * focus_dist,
                                        orig_eye[1] + out.forward[1] * focus_dist,
                                        orig_eye[2] + out.forward[2] * focus_dist};
        // Save the left-handed right axis (up x forward) to preserve camera roll.
        const std::array<double, 3> old_right =
            normalise({out.up[1] * out.forward[2] - out.up[2] * out.forward[1],
                       out.up[2] * out.forward[0] - out.up[0] * out.forward[2],
                       out.up[0] * out.forward[1] - out.up[1] * out.forward[0]});
        out.eye[1] = min_y;
        std::array<double, 3> new_dir{aim[0] - out.eye[0], aim[1] - out.eye[1], aim[2] - out.eye[2]};
        const double horiz = std::hypot(new_dir[0], new_dir[2]);
        if (horiz > 1e-4) {
          // Cap downward pitch at ~78 deg so forward stays well-conditioned against old_right.
          const double max_drop = horiz * 4.7;
          if (new_dir[1] < -max_drop) new_dir[1] = -max_drop;
          const std::array<double, 3> new_fwd = normalise(new_dir);
          const std::array<double, 3> new_up{new_fwd[1] * old_right[2] - new_fwd[2] * old_right[1],
                                             new_fwd[2] * old_right[0] - new_fwd[0] * old_right[2],
                                             new_fwd[0] * old_right[1] - new_fwd[1] * old_right[0]};
          const double up_len = std::sqrt(new_up[0] * new_up[0] + new_up[1] * new_up[1] + new_up[2] * new_up[2]);
          if (up_len > 1e-4) {
            out.forward = new_fwd;
            out.up = {new_up[0] / up_len, new_up[1] / up_len, new_up[2] / up_len};
          }
        }
      }
    }
  }
  return true;
}

bool load_freeroam_cutscenes(const std::filesystem::path& game_root, Library& out, std::string* error) {
  std::string why;
  auto modes = fh1::Archive::open(game_root / "media" / "gamemodes.zip", &why);
  if (!modes) {
    if (error) *error = "gamemodes.zip: " + why;
    return false;
  }
  auto bytes = modes->read(kFreeRoamCutscenesMember);
  if (!bytes.ok()) {
    if (error) *error = std::string("gamemodes.zip ") + kFreeRoamCutscenesMember + ": " + bytes.error;
    return false;
  }
  return parse_cutscenes(std::string_view(reinterpret_cast<const char*>(bytes.data.data()), bytes.data.size()), out,
                         error);
}

std::string track_cutscenes_member(std::string_view dev_name) {
  return "Colorado/Cutscenes/Tracks/cutscenes_" + std::string(dev_name) + ".xml";
}

bool load_gamemodes_cutscenes(const std::filesystem::path& game_root, std::string_view member, Library& out,
                              std::string* error) {
  std::string why;
  auto modes = fh1::Archive::open(game_root / "media" / "gamemodes.zip", &why);
  if (!modes) {
    if (error) *error = "gamemodes.zip: " + why;
    return false;
  }
  auto bytes = modes->read(std::string(member));
  if (!bytes.ok()) {
    if (error) *error = "gamemodes.zip " + std::string(member) + ": " + bytes.error;
    return false;
  }
  Library more;
  if (!parse_cutscenes(std::string_view(reinterpret_cast<const char*>(bytes.data.data()), bytes.data.size()), more,
                       error))
    return false;
  for (auto& c : more.cutscenes) out.cutscenes.push_back(std::move(c));
  return true;
}

const Cutscene* find_race_cutscene(const Library& lib, std::string_view name, std::string_view dev_name) {
  if (!dev_name.empty())
    if (const Cutscene* c = lib.find(std::string(name) + "_" + std::string(dev_name))) return c;
  return lib.find(name);
}

}  // namespace cutscene
