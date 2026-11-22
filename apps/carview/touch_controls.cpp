#include "touch_controls.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "ui4_chrome.h"

namespace carview {
namespace {

std::string lower_s(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void draw_touch_pad(ui::List& l, const TouchRect& r, float w, float h, std::string_view label,
                    bool pressed, float fill_frac, uint32_t active_col, float s) {
  const float x0 = r.x0 * w, y0 = r.y0 * h, x1 = r.x1 * w, y1 = r.y1 * h;
  const uint32_t bg = pressed ? ui::fade(active_col, 0.34) : ui::fade(ui4::kBrushBlack, 0.78);
  const uint32_t border = pressed ? active_col : ui::rgba(225, 225, 225, 165);
  // Sharp FH1 charcoal chrome with distressed texture grit and pink/accent edge strip (zero rounded corners).
  l.rect(x0, y0, x1, y1, bg);
  std::array<float, 4> uv;
  if (ui4::texture("DIRTMASKS/BGNOISE01", uv)) {
    l.image(x0, y0, x1, y1, uv, ui::rgba(255, 255, 255, pressed ? 24 : 18));
  }
  if (fill_frac > 0.01f) {
    const float fh = (y1 - y0 - 4 * s) * std::clamp(fill_frac, 0.0f, 1.0f);
    l.rect(x0 + 2 * s, y1 - 2 * s - fh, x1 - 2 * s, y1 - 2 * s, ui::fade(active_col, 0.56));
  }
  // Top accent bar (FH1 street-poster stripe)
  l.rect(x0, y0, x1, y0 + 3.0f * s, pressed ? active_col : ui::fade(active_col, 0.72));
  l.frame(x0, y0, x1, y1, border, (pressed ? 2.0f : 1.2f) * s);
  ui::TextStyle ts = ui4::style(ui::Font::kE, 15 * s, pressed ? ui4::kWhite : ui::rgba(240, 240, 240, 238),
                                ui::Align::kCenter);
  ui4::fit_text(label, ts, (x1 - x0) - 10 * s, 9 * s);
  l.text(0.5f * (x0 + x1), 0.5f * (y0 + y1) - 0.5f * ts.size, label, ts);
}

void draw_touch_chip(ui::List& l, const TouchRect& r, float w, float h, std::string_view label,
                     bool pressed, uint32_t accent, float s) {
  const float x0 = r.x0 * w, y0 = r.y0 * h, x1 = r.x1 * w, y1 = r.y1 * h;
  l.rect(x0, y0, x1, y1, pressed ? accent : ui::fade(ui4::kPillGrey, 0.88));
  l.rect(x0, y1 - 2.5f * s, x1, y1, pressed ? ui4::kWhite : accent);
  l.frame(x0, y0, x1, y1, pressed ? ui4::kWhite : ui::rgba(190, 190, 190, 130), 1.0f * s);
  ui::TextStyle ts = ui4::style(ui::Font::kE, 11.5f * s, ui4::kWhite, ui::Align::kCenter);
  ui4::fit_text(label, ts, (x1 - x0) - 8 * s, 8 * s);
  l.text(0.5f * (x0 + x1), 0.5f * (y0 + y1) - 0.5f * ts.size, label, ts);
}

}  // namespace

const char* touch_steer_mode_name(TouchSteerMode m) {
  switch (m) {
    case TouchSteerMode::kButtons: return "BTN";
    case TouchSteerMode::kThumbstick: return "STICK";
    case TouchSteerMode::kTilt: return "TILT";
    default: return "BTN";
  }
}

bool parse_touch_visibility(const std::string& s, TouchVisibility& out) {
  const std::string l = lower_s(s);
  if (l == "auto") { out = TouchVisibility::kAuto; return true; }
  if (l == "on" || l == "always" || l == "1") { out = TouchVisibility::kAlwaysOn; return true; }
  if (l == "off" || l == "0") { out = TouchVisibility::kOff; return true; }
  return false;
}

bool parse_touch_steer_mode(const std::string& s, TouchSteerMode& out) {
  const std::string l = lower_s(s);
  if (l == "buttons" || l == "btn") { out = TouchSteerMode::kButtons; return true; }
  if (l == "stick" || l == "thumbstick") { out = TouchSteerMode::kThumbstick; return true; }
  if (l == "tilt" || l == "gyro") { out = TouchSteerMode::kTilt; return true; }
  return false;
}

void TouchControls::cycle_steer_mode() {
  steer_mode_ = TouchSteerMode((int(steer_mode_) + 1) % 3);
}

TouchLayout TouchControls::compute_layout(float screen_w, float screen_h) {
  TouchLayout lay;
  const float aspect = (screen_h > 1.0f) ? (screen_w / screen_h) : (16.0f / 9.0f);
  // Wide phones (20:9+) get extra horizontal cutout/notch safe margins; tablets (4:3) stay compact vertically.
  const float mx = aspect >= 2.0f ? 0.045f : (aspect <= 1.5f ? 0.025f : 0.030f);
  const float my = aspect <= 1.5f ? 0.036f : 0.040f;
  const float pad_w = aspect >= 2.0f ? 0.086f : (aspect <= 1.5f ? 0.110f : 0.095f);
  const float pad_h = aspect <= 1.5f ? 0.215f : 0.240f;

  // Bottom-left steering buttons (compact so minimap sits cleanly above with zero overlap)
  const float sy1 = 1.0f - my;
  const float sy0 = sy1 - pad_h * 0.72f;
  lay.steer_left = {mx, sy0, mx + pad_w * 1.05f, sy1};
  lay.steer_right = {mx + pad_w * 1.15f, sy0, mx + pad_w * 2.20f, sy1};
  lay.steer_zone = {0.0f, 0.35f, 0.45f, 1.0f};

  // Bottom-right pedals: Throttle (outer right) & Brake (inner left of throttle)
  const float tx1 = 1.0f - mx;
  const float tx0 = tx1 - pad_w;
  lay.throttle = {tx0, 1.0f - my - pad_h, tx1, 1.0f - my};

  const float bx1 = tx0 - 0.014f;
  const float bx0 = bx1 - pad_w * 0.92f;
  lay.brake = {bx0, 1.0f - my - pad_h * 0.84f, bx1, 1.0f - my};

  // Handbrake above brake pedal
  const float hbh = 0.082f;
  lay.handbrake = {bx0, lay.brake.y0 - 0.016f - hbh, bx1, lay.brake.y0 - 0.016f};

  // Shift Up (+) / Shift Down (-) beside pedals (left of brake/handbrake)
  const float sh_w = pad_w * 0.56f;
  const float sh_h = 0.082f;
  const float sh_x1 = bx0 - 0.014f;
  const float sh_x0 = sh_x1 - sh_w;
  lay.shift_up = {sh_x0, lay.brake.y0 - 0.012f, sh_x1, lay.brake.y0 - 0.012f + sh_h};
  lay.shift_down = {sh_x0, lay.shift_up.y1 + 0.014f, sh_x1, lay.shift_up.y1 + 0.014f + sh_h};

  // Top center quick-action bar (stays clear of minimap and tachometer)
  const float top_y0 = my;
  const float top_y1 = my + 0.064f;
  const float cw = aspect >= 2.0f ? 0.064f : 0.072f;
  const float cg = 0.010f;
  const float group_w = 6.0f * cw + 5.0f * cg;
  const float gx0 = 0.5f - group_w * 0.5f;
  lay.btn_mode   = {gx0 + 0 * (cw + cg), top_y0, gx0 + 0 * (cw + cg) + cw, top_y1};
  lay.btn_rewind = {gx0 + 1 * (cw + cg), top_y0, gx0 + 1 * (cw + cg) + cw, top_y1};
  lay.btn_camera = {gx0 + 2 * (cw + cg), top_y0, gx0 + 2 * (cw + cg) + cw, top_y1};
  lay.btn_radio  = {gx0 + 3 * (cw + cg), top_y0, gx0 + 3 * (cw + cg) + cw, top_y1};
  lay.btn_map    = {gx0 + 4 * (cw + cg), top_y0, gx0 + 4 * (cw + cg) + cw, top_y1};
  lay.btn_pause  = {gx0 + 5 * (cw + cg), top_y0, gx0 + 5 * (cw + cg) + cw, top_y1};
  return lay;
}

bool TouchControls::rects_overlap(const TouchRect& a, const TouchRect& b) {
  return a.x0 < b.x1 && a.x1 > b.x0 && a.y0 < b.y1 && a.y1 > b.y0;
}

void TouchControls::hud_safe_rects(float screen_w, float screen_h, TouchRect& minimap_out, TouchRect& tach_out) {
  const TouchLayout lay = compute_layout(screen_w, screen_h);
  const float aspect = (screen_h > 1.0f) ? (screen_w / screen_h) : (16.0f / 9.0f);
  const float mx = lay.steer_left.x0;
  const float gap = 0.028f;

  // Minimap: sits cleanly ABOVE steer_left / steer_right on the left side.
  const float mm_y1 = lay.steer_left.y0 - gap;
  const float mm_h = std::min(0.24f, std::max(0.14f, mm_y1 - (lay.btn_mode.y1 + 0.04f)));
  const float mm_w = mm_h / std::max(0.5f, aspect);
  minimap_out = {mx, mm_y1 - mm_h, mx + mm_w, mm_y1};

  // Tachometer: sits cleanly ABOVE handbrake / throttle / shift_up on the right side.
  const float right_top = std::min({lay.handbrake.y0, lay.throttle.y0, lay.shift_up.y0});
  const float tach_y1 = right_top - gap;
  const float tach_h = std::min(0.25f, std::max(0.16f, tach_y1 - (lay.btn_pause.y1 + 0.04f)));
  const float tach_w = (tach_h / std::max(0.5f, aspect)) * 1.12f;
  const float tx1 = lay.throttle.x1;
  tach_out = {tx1 - tach_w, tach_y1 - tach_h, tx1, tach_y1};
}

bool TouchControls::handle_sdl_event(const SDL_Event& e, float screen_w, float screen_h, double now_s) {
  if (visibility_ == TouchVisibility::kOff) return false;
  if (e.type == SDL_EVENT_FINGER_DOWN) {
    return on_finger_down(int64_t(e.tfinger.fingerID), e.tfinger.x, e.tfinger.y, screen_w, screen_h, now_s);
  }
  if (e.type == SDL_EVENT_FINGER_MOTION) {
    return on_finger_motion(int64_t(e.tfinger.fingerID), e.tfinger.x, e.tfinger.y, screen_w, screen_h, now_s);
  }
  if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
    return on_finger_up(int64_t(e.tfinger.fingerID), now_s);
  }
  return false;
}

bool TouchControls::on_finger_down(int64_t finger_id, float nx, float ny, float screen_w, float screen_h,
                                   double now_s) {
  if (visibility_ == TouchVisibility::kOff) return false;
  nx = std::clamp(nx, 0.0f, 1.0f);
  ny = std::clamp(ny, 0.0f, 1.0f);
  ever_touched_ = true;
  last_touch_s_ = now_s;

  const TouchLayout lay = compute_layout(screen_w, screen_h);
  Role role = Role::kNone;
  if (lay.btn_pause.contains(nx, ny)) { role = Role::kPause; pending_actions_.pause = true; }
  else if (lay.btn_map.contains(nx, ny)) { role = Role::kMap; pending_actions_.map = true; }
  else if (lay.btn_camera.contains(nx, ny)) { role = Role::kCamera; pending_actions_.camera = true; }
  else if (lay.btn_rewind.contains(nx, ny)) { role = Role::kRewind; pending_actions_.rewind = true; }
  else if (lay.btn_radio.contains(nx, ny)) { role = Role::kRadio; pending_actions_.radio_next = true; }
  else if (lay.btn_mode.contains(nx, ny)) { role = Role::kMode; cycle_steer_mode(); }
  else if (lay.throttle.contains(nx, ny)) role = Role::kThrottle;
  else if (lay.brake.contains(nx, ny)) role = Role::kBrake;
  else if (lay.handbrake.contains(nx, ny)) role = Role::kHandbrake;
  else if (lay.shift_up.contains(nx, ny)) { role = Role::kShiftUp; pending_actions_.shift_up = true; }
  else if (lay.shift_down.contains(nx, ny)) { role = Role::kShiftDown; pending_actions_.shift_down = true; }
  else if (steer_mode_ == TouchSteerMode::kButtons &&
           (lay.steer_left.contains(nx, ny) || lay.steer_right.contains(nx, ny))) {
    role = Role::kSteerButtons;
  } else if (steer_mode_ == TouchSteerMode::kThumbstick && lay.steer_zone.contains(nx, ny)) {
    role = Role::kThumbstick;
  }

  // Remove any stale entry for same finger id
  fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(),
                                [&](const Finger& f) { return f.id == finger_id; }),
                 fingers_.end());
  fingers_.push_back({finger_id, role, nx, ny, nx, ny});
  recompute_output(screen_w, screen_h);
  return role != Role::kNone;
}

bool TouchControls::on_finger_motion(int64_t finger_id, float nx, float ny, float screen_w, float screen_h,
                                     double now_s) {
  if (visibility_ == TouchVisibility::kOff) return false;
  nx = std::clamp(nx, 0.0f, 1.0f);
  ny = std::clamp(ny, 0.0f, 1.0f);
  last_touch_s_ = now_s;
  for (auto& f : fingers_) {
    if (f.id == finger_id) {
      f.x = nx;
      f.y = ny;
      recompute_output(screen_w, screen_h);
      return f.role != Role::kNone;
    }
  }
  return false;
}

bool TouchControls::on_finger_up(int64_t finger_id, double now_s) {
  if (visibility_ == TouchVisibility::kOff) return false;
  last_touch_s_ = now_s;
  const size_t before = fingers_.size();
  fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(),
                                [&](const Finger& f) { return f.id == finger_id; }),
                 fingers_.end());
  if (fingers_.size() != before) {
    recompute_output(1280.0f, 720.0f);
    return true;
  }
  return false;
}

void TouchControls::set_tilt_roll(float roll_rad, float deadzone_rad, float max_rad) {
  const float a = std::fabs(roll_rad);
  if (a <= deadzone_rad) {
    tilt_steer_ = 0.0f;
  } else {
    const float mag = std::clamp((a - deadzone_rad) / std::max(0.05f, max_rad - deadzone_rad), 0.0f, 1.0f);
    tilt_steer_ = roll_rad >= 0.0f ? mag : -mag;
  }
  if (steer_mode_ == TouchSteerMode::kTilt) {
    out_.steer = double(tilt_steer_);
    if (std::fabs(tilt_steer_) > 0.01f) out_.active = true;
  }
}

void TouchControls::notify_gamepad_input(double now_s) {
  last_gamepad_s_ = now_s;
  gamepad_connected_ = true;
}

void TouchControls::set_gamepad_connected(bool connected) { gamepad_connected_ = connected; }

void TouchControls::update(double now_s) {
  if (steer_mode_ == TouchSteerMode::kTilt && visible(now_s)) {
    out_.steer = double(tilt_steer_);
    out_.active = !fingers_.empty() || std::fabs(tilt_steer_) > 0.01f;
  }
}

bool TouchControls::visible(double now_s) const {
  if (visibility_ == TouchVisibility::kOff) return false;
  if (visibility_ == TouchVisibility::kAlwaysOn) return true;
  if (!ever_touched_) return false;
  // Auto-hide immediately when gamepad was used more recently than last touch.
  if (gamepad_connected_ && last_gamepad_s_ >= last_touch_s_) return false;
  return (now_s - last_touch_s_) <= kAutoHideSeconds;
}

TouchDriveOutput TouchControls::output() const { return out_; }

TouchActions TouchControls::consume_actions() {
  TouchActions a = pending_actions_;
  pending_actions_ = {};
  return a;
}

void TouchControls::recompute_output(float screen_w, float screen_h) {
  const TouchLayout lay = compute_layout(screen_w, screen_h);
  out_ = TouchDriveOutput{};
  double steer = (steer_mode_ == TouchSteerMode::kTilt) ? double(tilt_steer_) : 0.0;
  double throttle = 0.0;
  double brake = 0.0;
  bool hb = false;
  bool any = (steer_mode_ == TouchSteerMode::kTilt && std::fabs(tilt_steer_) > 0.01f);

  for (const auto& f : fingers_) {
    if (f.role == Role::kSteerButtons) {
      any = true;
      const float split_x = 0.5f * (lay.steer_left.x1 + lay.steer_right.x0);
      if (f.x < split_x) steer = -1.0;
      else steer = +1.0;
    } else if (f.role == Role::kThumbstick) {
      any = true;
      constexpr float kRadius = 0.095f;
      steer = std::clamp(double((f.x - f.anchor_x) / kRadius), -1.0, 1.0);
    } else if (f.role == Role::kThrottle) {
      any = true;
      // Vertical analog pedal: bottom 30% = 0.65, sliding up reaches 1.00.
      const float frac = std::clamp((lay.throttle.y1 - f.y) / std::max(0.01f, lay.throttle.h()), 0.0f, 1.0f);
      throttle = std::max(throttle, 0.55 + 0.45 * double(frac));
    } else if (f.role == Role::kBrake) {
      any = true;
      const float frac = std::clamp((lay.brake.y1 - f.y) / std::max(0.01f, lay.brake.h()), 0.0f, 1.0f);
      brake = std::max(brake, 0.55 + 0.45 * double(frac));
    } else if (f.role == Role::kHandbrake) {
      any = true;
      hb = true;
    }
  }

  out_.active = any;
  out_.steer = std::clamp(steer, -1.0, 1.0);
  out_.throttle = std::clamp(throttle, 0.0, 1.0);
  out_.brake = std::clamp(brake, 0.0, 1.0);
  out_.handbrake = hb;
}

void TouchControls::build_overlay(ui::List& l, float screen_w, float screen_h, double now_s,
                                  bool demo_active) const {
  if (!demo_active && !visible(now_s)) return;
  const TouchLayout lay = compute_layout(screen_w, screen_h);
  const float s = std::min(screen_w / 1280.0f, screen_h / 720.0f);

  const double steer = demo_active ? -0.45 : out_.steer;
  const double thr = demo_active ? 0.88 : out_.throttle;
  const double brk = demo_active ? 0.0 : out_.brake;
  const bool hb = demo_active ? false : out_.handbrake;

  // Steering controls (left side)
  if (steer_mode_ == TouchSteerMode::kButtons) {
    draw_touch_pad(l, lay.steer_left, screen_w, screen_h, "<", steer < -0.1,
                   steer < -0.1 ? float(-steer) : 0.0f, ui4::kPink, s);
    draw_touch_pad(l, lay.steer_right, screen_w, screen_h, ">", steer > 0.1,
                   steer > 0.1 ? float(steer) : 0.0f, ui4::kPink, s);
  } else if (steer_mode_ == TouchSteerMode::kThumbstick) {
    float cx = (lay.steer_left.cx() + lay.steer_right.cx()) * 0.5f * screen_w;
    float cy = lay.steer_left.cy() * screen_h;
    float kx = cx + float(steer) * 62 * s;
    float ky = cy;
    for (const auto& f : fingers_) {
      if (f.role == Role::kThumbstick) {
        cx = f.anchor_x * screen_w;
        cy = f.anchor_y * screen_h;
        kx = cx + float(std::clamp(f.x - f.anchor_x, -0.095f, 0.095f)) * screen_w;
        ky = cy + float(std::clamp(f.y - f.anchor_y, -0.06f, 0.06f)) * screen_h;
        break;
      }
    }
    const float R = 68 * s;
    l.circle(cx, cy, R, ui::rgba(16, 18, 24, 130));
    l.arc(cx, cy, R, 2.2f * s, 0.0f, 6.28318f, ui::rgba(230, 236, 244, 170));
    l.circle(kx, ky, 26 * s, std::fabs(steer) > 0.05 ? ui::fade(ui4::kPink, 0.85) : ui::rgba(220, 224, 232, 165));
  } else {
    // Tilt steering indicator bar (sharp FH1 strip)
    const float tx0 = lay.steer_left.x0 * screen_w;
    const float tx1 = lay.steer_right.x1 * screen_w;
    const float ty0 = lay.steer_left.cy() * screen_h - 14 * s;
    const float ty1 = ty0 + 28 * s;
    l.rect(tx0, ty0, tx1, ty1, ui::fade(ui4::kBrushBlack, 0.85));
    l.frame(tx0, ty0, tx1, ty1, ui4::kPink, 1.5f * s);
    const float mid = 0.5f * (tx0 + tx1);
    const float kx = mid + float(steer) * (0.5f * (tx1 - tx0) - 10 * s);
    l.rect(kx - 4 * s, ty0 + 3 * s, kx + 4 * s, ty1 - 3 * s, ui4::kPink);
    l.text(mid, ty0 - 18 * s, "TILT STEERING", ui4::style(ui::Font::kB, 11 * s, ui4::kYellow, ui::Align::kCenter));
  }

  // Right-side pedals & paddles
  draw_touch_pad(l, lay.throttle, screen_w, screen_h, "GAS", thr > 0.05, float(thr), ui4::kGreen, s);
  draw_touch_pad(l, lay.brake, screen_w, screen_h, "BRK", brk > 0.05, float(brk), ui4::kRed, s);
  draw_touch_pad(l, lay.handbrake, screen_w, screen_h, "HB", hb, hb ? 1.0f : 0.0f, ui4::kAmber, s);

  bool su_p = false, sd_p = false;
  for (const auto& f : fingers_) {
    if (f.role == Role::kShiftUp) su_p = true;
    if (f.role == Role::kShiftDown) sd_p = true;
  }
  draw_touch_pad(l, lay.shift_up, screen_w, screen_h, "+", su_p, su_p ? 1.0f : 0.0f, ui4::kBlue, s);
  draw_touch_pad(l, lay.shift_down, screen_w, screen_h, "-", sd_p, sd_p ? 1.0f : 0.0f, ui4::kBlue, s);

  // Top quick-action bar
  draw_touch_chip(l, lay.btn_mode, screen_w, screen_h, touch_steer_mode_name(steer_mode_), false, ui4::kAmber, s);
  draw_touch_chip(l, lay.btn_rewind, screen_w, screen_h, "RW", false, ui4::kAmber, s);
  draw_touch_chip(l, lay.btn_camera, screen_w, screen_h, "CAM", false, ui4::kBlue, s);
  draw_touch_chip(l, lay.btn_radio, screen_w, screen_h, "RAD", false, ui4::kPink, s);
  draw_touch_chip(l, lay.btn_map, screen_w, screen_h, "MAP", false, ui4::kGreen, s);
  draw_touch_chip(l, lay.btn_pause, screen_w, screen_h, "II", false, ui4::kWhite, s);
}

}  // namespace carview
