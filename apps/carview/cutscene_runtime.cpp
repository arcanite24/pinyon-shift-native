#include "cutscene_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace carview {
namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

bool CutsceneRuntime::ensure_loaded(const std::filesystem::path& game_root) {
  if (tried_) return loaded_;
  tried_ = true;
  loaded_ = cutscene::load_freeroam_cutscenes(game_root, lib_, &error_);
  if (!loaded_) std::fprintf(stderr, "carview: cutscenes: %s\n", error_.c_str());
  return loaded_;
}

bool CutsceneRuntime::play(std::string_view name, bool hold) {
  const cutscene::Cutscene* cs = lib_.find(name);
  frozen_ = false;
  if (!cs) {
    std::fprintf(stderr, "carview: cutscene '%.*s' not found\n", int(name.size()), name.data());
    player_.stop();
    return false;
  }
  player_.start(cs);
  player_.set_hold(hold);
  // A cut to black ends where a cutscene takes over (its own fades apply).
  if (fade_on_ && (fade_.kind == race::FtcFadeKind::kBlackOut || fade_.kind == race::FtcFadeKind::kFadeOut) &&
      fade_t_ >= fade_.duration)
    fade_on_ = false;
  return true;
}

void CutsceneRuntime::stop() {
  player_.stop();
  frozen_ = false;
}

std::vector<const cutscene::Event*> CutsceneRuntime::update(double dt, bool vo_playing) {
  step_fades(dt);
  if (!player_.active() || frozen_) return {};
  player_.set_voice_playing(vo_playing);
  return player_.step(dt);
}

std::vector<const cutscene::Event*> CutsceneRuntime::seek_and_freeze(double t) {
  std::vector<const cutscene::Event*> out;
  if (!player_.active()) return out;
  auto first = player_.step(0.0);
  out.insert(out.end(), first.begin(), first.end());
  const double target = std::max(0.0, t - player_.time());
  auto rest = player_.step(target);
  out.insert(out.end(), rest.begin(), rest.end());
  frozen_ = true;
  fade_on_ = false;
  return out;
}

void CutsceneRuntime::fade(const race::FtcFade& f) {
  fade_ = f;
  if (fade_.kind == race::FtcFadeKind::kWhiteFlash) fade_.duration = kWhiteFlashSeconds;
  fade_on_ = true;
  fade_t_ = 0.0;
}

void CutsceneRuntime::step_fades(double dt) {
  if (!fade_on_) return;
  fade_t_ += std::max(0.0, dt);
  // Fades that end clear are dropped; black (CBlackOut, a finished CFadeOut)
  // stays until the next fade or cutscene.
  if ((fade_.kind == race::FtcFadeKind::kFadeIn || fade_.kind == race::FtcFadeKind::kWhiteFlash) &&
      fade_t_ >= fade_.duration)
    fade_on_ = false;
}

cutscene::Overlay CutsceneRuntime::overlay() const {
  cutscene::Overlay o;
  if (fade_on_) {
    cutscene::FadeOptions f;
    f.duration = fade_.duration;
    f.curve = fade_.s_curve ? cutscene::CurveType::kS : cutscene::CurveType::kLinear;
    switch (fade_.kind) {
      case race::FtcFadeKind::kBlackOut: o.alpha = 1.0f; break;
      case race::FtcFadeKind::kWhiteFlash:
        f.curve = cutscene::CurveType::kLinear;
        f.color = {1, 1, 1, 0};
        o = cutscene::fade_overlay(f, fade_t_, true);
        break;
      case race::FtcFadeKind::kFadeIn: o = cutscene::fade_overlay(f, fade_t_, true); break;
      case race::FtcFadeKind::kFadeOut: o = cutscene::fade_overlay(f, fade_t_, false); break;
    }
  }
  if (const auto* cs = current()) {
    const auto own = cutscene::cutscene_overlay(*cs, player_.time());
    if (own.alpha > o.alpha) o = own;
  }
  return o;
}

bool CutsceneRuntime::camera(const std::array<double, 3>& car_pos, double car_heading_rad,
                             drive::CameraView& out) const {
  if (!player_.active()) return false;
  cutscene::WorldCamera w;
  const cutscene::Frame car{car_pos, car_heading_rad * 180.0 / kPi};
  if (!player_.world_camera(car, w)) return false;
  out.eye = w.eye;
  out.forward = w.forward;
  out.up = w.up;
  out.fov_degrees = w.fov_deg;  // vertical (interim, Q61 b)
  out.near_clip = 0.1;
  out.interior = w.interior;
  out.mode = w.interior ? drive::CameraMode::kCockpit : drive::CameraMode::kFollowLow;
  return true;
}

bool CutsceneRuntime::streaming_focus(const std::array<double, 3>& car_pos, double car_heading_rad,
                                      std::array<double, 3>& out) const {
  drive::CameraView v;
  if (!camera(car_pos, car_heading_rad, v)) return false;
  out = v.eye;
  return true;
}

}  // namespace carview
