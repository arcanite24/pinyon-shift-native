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
  std::vector<SDL_Joystick*> wheel_joysticks_;
  std::string ffb_device_match_;

  void set_window(SDL_Window* window) {
    native_hwnd_ = nullptr;
    if (!window) return;
#ifdef _WIN32
    native_hwnd_ =
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
  }

  void ensure_ffb_open() {
    if (ffb_cfg.mode == drive::WheelFfbMode::kOff) {
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

  void sync_wheel_devices() {
    for (SDL_Joystick* j : wheel_joysticks_) {
      if (j) SDL_CloseJoystick(j);
    }
    wheel_joysticks_.clear();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (ids) {
      for (int i = 0; i < count; ++i) {
        if (SDL_IsGamepad(ids[i])) continue;
        if (SDL_Joystick* j = SDL_OpenJoystick(ids[i])) {
          wheel_joysticks_.push_back(j);
        }
      }
      SDL_free(ids);
    }
    ensure_ffb_open();
  }

  // Applies single-device wheel shaping (for raw wheel joysticks) and DualSense/DS4/SwitchPro
  // gyro tilt steering into the live driving Input struct.
  void apply_wheel_and_gyro_input(drive::Input& in, bool pad_has_input) const {
    if (!pad_has_input) {
      for (SDL_Joystick* j : wheel_joysticks_) {
        if (!j) continue;
        const int n_axes = SDL_GetNumJoystickAxes(j);
        if (n_axes <= 0) continue;
        const double raw_s = std::clamp(SDL_GetJoystickAxis(j, 0) / 32767.0, -1.0, 1.0);
        const double shaped_s = drive::shape_wheel_steering(raw_s, wheel_shaping);
        double shaped_t = 0.0, shaped_b = 0.0;
        if (n_axes >= 2) {
          const double raw_t = std::clamp((32767.0 - double(SDL_GetJoystickAxis(j, 1))) / 65535.0, 0.0, 1.0);
          shaped_t = drive::shape_wheel_pedal(raw_t, wheel_shaping);
        }
        if (n_axes >= 3) {
          const double raw_b = std::clamp((32767.0 - double(SDL_GetJoystickAxis(j, 2))) / 65535.0, 0.0, 1.0);
          shaped_b = drive::shape_wheel_pedal(raw_b, wheel_shaping);
        }
        if (std::fabs(shaped_s) > 1e-3 || shaped_t > 1e-3 || shaped_b > 1e-3) {
          in.steer = shaped_s;
          if (shaped_t > in.throttle) in.throttle = shaped_t;
          if (shaped_b > in.brake) in.brake = shaped_b;
          break;
        }
      }
    }
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

  void sync_pad_capabilities(const std::vector<SDL_Gamepad*>& pads) {
    bool ps_active = false;
    for (SDL_Gamepad* pad : pads) {
      if (!pad) continue;
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
    ui4::set_glyph_set(ps_active ? ui4::GlyphSet::kPlayStation : ui4::GlyphSet::kXbox);
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
    const auto ffb_tel = drive::ffb_telemetry_from_vehicle(v, active_drive);
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
    for (SDL_Joystick* j : wheel_joysticks_) {
      if (j) SDL_CloseJoystick(j);
    }
    wheel_joysticks_.clear();
    ffb_trace.stop();
    ht_receiver.stop();
    tel_sender.close();
    tel_probe.stop();
  }
};

}  // namespace carview
