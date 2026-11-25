// Modular runtime bridge for Slice 12 peripherals, camera tools, UDP
// telemetry, F10 Trainer, Festival Pacing, and Hidden Cars. Keeps
// showroom_main.cpp edits minimal and conflict-free.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "camera_editor.h"
#include "drive/cameras.h"
#include "drive/dualsense.h"
#include "drive/head_tracking.h"
#include "drive/telemetry_udp.h"
#include "drive/trainer.h"
#include "drive/vehicle.h"
#include "drive/wheel_ffb.h"
#include "gamepad_db.h"
#include "settings_page.h"
#include "trainer_overlay.h"
#include "ui4_chrome.h"

namespace carview {

struct Slice12Runtime {
  // Settings & models.
  drive::WheelShaping wheel_shaping;
  drive::WheelFfbConfig ffb_cfg;
  drive::WheelFfbSolver ffb_solver;
  drive::DirectInputFfbDevice ffb_device;
  drive::FfbTraceRecorder ffb_trace;

  drive::dualsense::Settings ds_cfg;
  drive::dualsense::TriggerModel ds_model;
  drive::dualsense::EffectChannel ds_channel;
  drive::dualsense::GyroTiltSteering gyro_steer;
  drive::dualsense::Rgb last_led_{0, 0, 0};
  bool led_set_ = false;
  bool window_focused_ = true;

  drive::HeadTrackingConfig ht_cfg;
  drive::HeadTrackingReceiver ht_receiver;

  drive::CameraEditorConfig cam_editor_cfg;
  drive::ImmersiveCameraConfig imm_cam_cfg;
  CameraEditorOverlay cam_editor_overlay;

  drive::telemetry::Config tel_cfg;
  drive::telemetry::Sampler tel_sampler;
  drive::telemetry::UdpSender tel_sender;
  drive::telemetry::ProbeRecorder tel_probe;

  drive::TrainerState trainer;
  TrainerOverlay trainer_overlay;
  std::string save_tree_label = "user";
  void* native_hwnd_ = nullptr;
  std::vector<RawJoystickEntry> wheel_joysticks_;
  std::vector<RawJoystickEntry> raw_joysticks_;
  std::string ffb_device_match_;
  bool last_input_was_keyboard_ = false;

  static ui4::GlyphSet glyph_set_for_gamepad(uint16_t vendor_id, uint16_t product_id) {
    return drive::dualsense::is_playstation_pad(vendor_id, product_id) ? ui4::GlyphSet::kPlayStation
                                                                       : ui4::GlyphSet::kXbox;
  }

  void notify_keyboard_or_mouse_input() {
    last_input_was_keyboard_ = true;
    ui4::set_glyph_set(ui4::GlyphSet::kKeyboard);
  }

  void notify_gamepad_input(SDL_JoystickID which, const std::vector<SDL_Gamepad*>& pads) {
    last_input_was_keyboard_ = false;
    SDL_Gamepad* matched = nullptr;
    if (which != 0) {
      for (SDL_Gamepad* pad : pads) {
        if (pad && SDL_GetGamepadID(pad) == which) {
          matched = pad;
          break;
        }
      }
    }
    if (!matched && !pads.empty()) matched = pads.front();
    if (matched) {
      ui4::set_glyph_set(glyph_set_for_gamepad(SDL_GetGamepadVendor(matched), SDL_GetGamepadProduct(matched)));
    } else {
      ui4::set_glyph_set(ui4::GlyphSet::kXbox);
    }
  }

  void set_window(SDL_Window* window) {
    native_hwnd_ = nullptr;
    if (!window) return;
#ifdef _WIN32
    native_hwnd_ =
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
  }

  void ensure_ffb_open() {
    if (ffb_cfg.mode == drive::WheelFfbMode::kOff || wheel_joysticks_.empty()) {
      if (ffb_device.is_open()) {
        ffb_device.stop_all();
        ffb_device.close();
      }
      return;
    }
    if (!ffb_device.is_open() && native_hwnd_) {
      ffb_device.open(native_hwnd_, ffb_device_match_);
    }
  }

  void close_non_gamepad_joysticks() {
    for (auto& w : wheel_joysticks_) {
      if (w.joy) SDL_CloseJoystick(w.joy);
    }
    wheel_joysticks_.clear();
    for (auto& r : raw_joysticks_) {
      if (r.joy) SDL_CloseJoystick(r.joy);
    }
    raw_joysticks_.clear();
  }

  void sync_wheel_devices() {
    close_non_gamepad_joysticks();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (ids) {
      SDL_UpdateJoysticks();
      for (int i = 0; i < count; ++i) {
        if (SDL_IsGamepad(ids[i])) continue;
        if (SDL_Joystick* j = SDL_OpenJoystick(ids[i])) {
          RawJoystickEntry entry;
          entry.joy = j;
          entry.id = ids[i];
          const int n_axes = std::max(0, SDL_GetNumJoystickAxes(j));
          entry.rest_axes.resize(size_t(n_axes));
          for (int a = 0; a < n_axes; ++a) entry.rest_axes[size_t(a)] = SDL_GetJoystickAxis(j, a);
          const int n_btns = std::max(0, SDL_GetNumJoystickButtons(j));
          entry.prev_buttons.assign(size_t(n_btns), 0u);
          if (SDL_GetNumJoystickHats(j) > 0) entry.prev_hat = SDL_GetJoystickHat(j, 0);
          const uint16_t vid = SDL_GetJoystickVendor(j);
          const uint16_t pid = SDL_GetJoystickProduct(j);
          const char* raw_name = SDL_GetJoystickName(j);
          const std::string_view jname = raw_name ? std::string_view(raw_name) : std::string_view{};
          entry.is_wheel = is_racing_wheel_device(vid, pid, jname);
          if (entry.is_wheel) {
            wheel_joysticks_.push_back(std::move(entry));
          } else {
            raw_joysticks_.push_back(std::move(entry));
            std::printf("joystick fallback: %s (vid=%04x pid=%04x, %d axes, %d buttons)\n",
                        raw_name ? raw_name : "(unnamed)", unsigned(vid), unsigned(pid), n_axes, n_btns);
          }
        }
      }
      SDL_free(ids);
    }
    ensure_ffb_open();
  }

  bool has_any_raw_controller() const { return !raw_joysticks_.empty() || !wheel_joysticks_.empty(); }

  // Samples the first active unmapped (non-SDL_Gamepad) USB/BT controller.
  RawJoystickDriveOutput sample_raw_joystick_input(const drive::ControlDeadzones& dz) const {
    for (const auto& entry : raw_joysticks_) {
      if (!entry.joy) continue;
      const auto snap = snapshot_raw_joystick(entry);
      const auto sampled = sample_raw_joystick_pad(snap, dz);
      if (sampled.any_active) return sampled;
    }
    return {};
  }

  // Samples the first active genuine racing wheel (calibrated resting pedals).
  RawJoystickDriveOutput sample_wheel_input() const {
    for (const auto& entry : wheel_joysticks_) {
      if (!entry.joy) continue;
      const auto snap = snapshot_raw_joystick(entry);
      const auto sampled = sample_wheel_joystick(snap, wheel_shaping);
      if (sampled.any_active) return sampled;
    }
    return {};
  }

  // Applies DualSense/DS4/SwitchPro gyro tilt steering into the live driving Input struct.
  void apply_gyro_input(drive::Input& in) const {
    if (ds_cfg.gyro_steering && gyro_steer.valid()) {
      const double gsteer = double(gyro_steer.steering(ds_cfg.gyro_sensitivity_pct));
      if (std::fabs(gsteer) > 1e-4) {
        in.steer = std::clamp(in.steer + gsteer, -1.0, 1.0);
      }
    }
  }

  void apply_live_settings(const LiveSettings& ls, drive::DriveCamera& rig, const std::filesystem::path& log_dir) {
    wheel_shaping = ls.wheel_shaping;
    ffb_cfg = ls.wheel_ffb;
    ds_cfg = ls.dualsense;
    ht_cfg = ls.head_tracking;
    ht_receiver.configure(ht_cfg);

    cam_editor_cfg = ls.camera_editor;
    imm_cam_cfg = ls.immersive_camera;
    rig.set_camera_editor(cam_editor_cfg);
    rig.set_immersive_camera(imm_cam_cfg);

    tel_cfg = ls.telemetry_udp;
    trainer.config.cheats_enabled = ls.trainer_cheats;
    if (ls.trainer_saved_road.has_value()) trainer.config.saved_map_road = ls.trainer_saved_road;
    trainer.pacing = ls.festival_pacing;
    trainer.hidden_cars = ls.hidden_cars;

    ensure_ffb_open();

    if (trainer.config.debug_ffb_trace && !ffb_trace.is_recording()) {
      ffb_trace.start(log_dir);
    } else if (!trainer.config.debug_ffb_trace && ffb_trace.is_recording()) {
      ffb_trace.stop();
    }
    if (trainer.config.debug_telemetry_probe && !tel_probe.is_recording()) {
      tel_probe.start(log_dir / "telemetry-probe.csv");
    } else if (!trainer.config.debug_telemetry_probe && tel_probe.is_recording()) {
      tel_probe.stop();
    }
  }

  void sync_pad_capabilities(const std::vector<SDL_Gamepad*>& pads, bool force_gamepad = false) {
    bool ps_active = false;
    bool any_pad = false;
    for (SDL_Gamepad* pad : pads) {
      if (!pad) continue;
      any_pad = true;
      const uint16_t vid = SDL_GetGamepadVendor(pad);
      const uint16_t pid = SDL_GetGamepadProduct(pad);
      if (drive::dualsense::is_playstation_pad(vid, pid)) ps_active = true;
      if (drive::dualsense::supports_gyro_tilt(vid, pid)) {
        const bool want_gyro = ds_cfg.gyro_steering;
        if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, want_gyro);
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, want_gyro);
        }
      }
    }
    if (!any_pad && !has_any_raw_controller()) {
      last_input_was_keyboard_ = true;
      ui4::set_glyph_set(ui4::GlyphSet::kKeyboard);
    } else if (force_gamepad || !last_input_was_keyboard_) {
      last_input_was_keyboard_ = false;
      ui4::set_glyph_set(ps_active ? ui4::GlyphSet::kPlayStation : ui4::GlyphSet::kXbox);
    }
  }

  void on_sdl_event(const SDL_Event& e, const std::vector<SDL_Gamepad*>& pads) {
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
      window_focused_ = false;
      release_all(pads);
    } else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
      window_focused_ = true;
    } else if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && ds_cfg.gyro_steering) {
      if (e.gsensor.sensor == SDL_SENSOR_ACCEL) {
        gyro_steer.on_accel(e.gsensor.data[0], e.gsensor.data[1], e.gsensor.data[2]);
      } else if (e.gsensor.sensor == SDL_SENSOR_GYRO) {
        gyro_steer.on_gyro(e.gsensor.data[0], e.gsensor.data[1], e.gsensor.data[2], e.gsensor.sensor_timestamp);
      }
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ds_cfg.gyro_steering) {
      if (e.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD || e.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1) {
        gyro_steer.recenter();
      }
    }
  }

  void tick(const drive::Vehicle& v, drive::DriveCamera& rig, const std::vector<SDL_Gamepad*>& pads,
            bool driving_active, double dt_s, uint64_t now_ns, uint32_t timestamp_ms,
            double rumble_low = 0.0, double rumble_high = 0.0) {
    const bool active_drive = driving_active && window_focused_;

    // 1. OpenTrack head tracking -> DriveCamera cockpit/hood layer.
    if (ht_cfg.enabled) rig.set_head_pose(ht_receiver.current_pose());
    else rig.set_head_pose({});

    // 2. DirectInput FFB + CSV trace.
    const auto ffb_tel = drive::ffb_telemetry_from_vehicle(v, active_drive, v.lateral_g());
    const auto ffb_forces = ffb_solver.step(ffb_tel, ffb_cfg, dt_s);
    if (ffb_device.is_open()) ffb_device.apply(ffb_forces);
    if (ffb_trace.is_recording()) ffb_trace.record(v.time(), ffb_cfg.mode, ffb_tel, ffb_forces);

    // 3. DualSense adaptive triggers & RPM/Player lightbar.
    const auto ds_tel = drive::dualsense::telemetry_from_vehicle(v);
    drive::dualsense::TriggerInputs ti;
    ti.driving = active_drive;
    ti.brake = static_cast<float>(v.shaped_input().brake);
    ti.throttle = static_cast<float>(v.shaped_input().throttle);
    ti.motor_low = static_cast<float>(rumble_low);
    ti.motor_high = static_cast<float>(rumble_high);
    ti.telemetry = &ds_tel;
    const auto wanted_fx = ds_model.step(ti, ds_cfg, dt_s);
    drive::dualsense::EffectPayload payload{};
    const bool send_fx = ds_channel.update(wanted_fx, now_ns, &payload);
    const bool blink = ((now_ns / 80000000ULL) & 1ULL) == 0ULL;
    const auto led = drive::dualsense::lightbar_colour(ds_cfg.lightbar_mode, 0, ds_tel.rpm, ds_tel.redline_rpm,
                                                       active_drive, blink);
    const bool send_led = !led_set_ || led != last_led_;
    for (SDL_Gamepad* pad : pads) {
      if (!pad) continue;
      if (!drive::dualsense::is_dualsense(SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad))) continue;
      if (send_fx) SDL_SendGamepadEffect(pad, payload.data(), int(payload.size()));
      if (send_led) SDL_SetGamepadLED(pad, led.r, led.g, led.b);
    }
    if (send_led) {
      last_led_ = led;
      led_set_ = true;
    }

    // 4. UDP Telemetry Out (FM7 311B / FH4/5 324B) + probe CSV.
    if (tel_cfg.enabled || tel_probe.is_recording()) {
      const auto sample = tel_sampler.build(v, active_drive, dt_s, tel_cfg.moving_only);
      if (tel_cfg.enabled) tel_sender.send(tel_cfg, sample, timestamp_ms);
      if (tel_probe.is_recording()) tel_probe.record(timestamp_ms, sample);
    }
  }

  void release_all(const std::vector<SDL_Gamepad*>& pads) {
    ffb_device.stop_all();
    const auto slack = ds_channel.force_release(0);
    for (SDL_Gamepad* pad : pads) {
      if (!pad) continue;
      if (drive::dualsense::is_dualsense(SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad))) {
        SDL_SendGamepadEffect(pad, slack.data(), int(slack.size()));
        SDL_SetGamepadLED(pad, 0, 64, 255);
      }
    }
  }

  void shutdown(const std::vector<SDL_Gamepad*>& pads) {
    release_all(pads);
    ffb_device.close();
    close_non_gamepad_joysticks();
    ffb_trace.stop();
    ht_receiver.stop();
    tel_sender.close();
    tel_probe.stop();
  }
};

}  // namespace carview
