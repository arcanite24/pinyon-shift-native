#include <SDL3/SDL.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "platform_support.h"
#include "showroom.h"
#include "touch_controls.h"

namespace {

void test_touch_layout_aspects() {
  // Verify safe normalized bounds across phone (20:9), handheld/Odin (16:9), and tablet (4:3).
  const std::pair<float, float> sizes[] = {{2400.0f, 1080.0f}, {1920.0f, 1080.0f}, {1280.0f, 720.0f}, {2048.0f, 1536.0f}};
  for (auto [w, h] : sizes) {
    const auto lay = carview::TouchControls::compute_layout(w, h);
    assert(lay.steer_left.x0 > 0.0f && lay.steer_left.x1 < lay.steer_right.x0);
    assert(lay.steer_right.x1 < 0.45f);
    assert(lay.brake.x1 < lay.throttle.x0 && lay.throttle.x1 < 1.0f);
    assert(lay.handbrake.y1 < lay.brake.y0);
    assert(lay.shift_up.x1 < lay.brake.x0);
    assert(lay.btn_mode.x0 > 0.1f && lay.btn_pause.x1 < 0.9f);

    // Phase 1 parity: minimap and tachometer safe rects must NEVER overlap any touch control.
    carview::TouchRect mm, tach;
    carview::TouchControls::hud_safe_rects(w, h, mm, tach);
    assert(mm.x0 >= 0.0f && mm.y0 >= 0.0f && mm.x1 <= 1.0f && mm.y1 <= 1.0f);
    assert(tach.x0 >= 0.0f && tach.y0 >= 0.0f && tach.x1 <= 1.0f && tach.y1 <= 1.0f);
    assert(!carview::TouchControls::rects_overlap(mm, tach));
    const carview::TouchRect pads[] = {lay.steer_left, lay.steer_right, lay.throttle,  lay.brake,
                                       lay.handbrake,  lay.shift_up,    lay.shift_down, lay.btn_mode,
                                       lay.btn_rewind, lay.btn_camera,  lay.btn_radio, lay.btn_map,
                                       lay.btn_pause};
    for (const auto& p : pads) {
      assert(!carview::TouchControls::rects_overlap(mm, p));
      assert(!carview::TouchControls::rects_overlap(tach, p));
    }

    // Android SETUP pill sits in the top-center safe bar and must NEVER overlap
    // top-right HUD (position/timer/playerbar), top-left HUD, minimap, tachometer, or touch pads.
    const carview::TouchRect setup_pill = carview::TouchControls::android_setup_pill_rect(w, h);
    const carview::TouchRect tr_hud = carview::TouchControls::top_right_hud_rect(w, h);
    const carview::TouchRect tl_hud = carview::TouchControls::top_left_hud_rect(w, h);
    assert(setup_pill.x0 > 0.30f && setup_pill.x1 < 0.70f);
    assert(setup_pill.y0 >= 0.0f && setup_pill.y1 < 0.10f);
    assert(std::fabs(setup_pill.cx() - 0.5f) < 1e-4f);
    assert(!carview::TouchControls::rects_overlap(setup_pill, tr_hud));
    assert(!carview::TouchControls::rects_overlap(setup_pill, tl_hud));
    assert(!carview::TouchControls::rects_overlap(setup_pill, mm));
    assert(!carview::TouchControls::rects_overlap(setup_pill, tach));
    for (const auto& p : pads) {
      assert(!carview::TouchControls::rects_overlap(setup_pill, p));
    }
  }
  std::printf("  [ok] touch layout scaling, Android SETUP pill & zero HUD/touch overlap across 20:9, 16:9, 4:3\n");
}

void test_touch_multitouch_buttons_and_pedals() {
  carview::TouchControls tc;
  const float w = 1280.0f, h = 720.0f;
  const auto lay = carview::TouchControls::compute_layout(w, h);

  // Initially hidden in Auto mode before first touch.
  assert(!tc.visible(0.0));

  // Finger 1 presses Left steer; Finger 2 presses Throttle (upper part of pad -> near 1.0).
  assert(tc.on_finger_down(1, lay.steer_left.cx(), lay.steer_left.cy(), w, h, 1.0));
  assert(tc.on_finger_down(2, lay.throttle.cx(), lay.throttle.y0 + 0.02f, w, h, 1.0));
  assert(tc.visible(1.0));

  auto out = tc.output();
  assert(out.active);
  assert(out.steer < -0.9);
  assert(out.throttle > 0.9);
  assert(out.brake == 0.0);

  // Slide finger 1 right into right-steer pad without lifting!
  assert(tc.on_finger_motion(1, lay.steer_right.cx(), lay.steer_right.cy(), w, h, 1.1));
  out = tc.output();
  assert(out.steer > 0.9);

  // Finger 3 hits Handbrake + Finger 4 hits Shift Up + Quick actions (Rewind / Camera / Pause / Map / Radio).
  assert(tc.on_finger_down(3, lay.handbrake.cx(), lay.handbrake.cy(), w, h, 1.2));
  assert(tc.on_finger_down(4, lay.shift_up.cx(), lay.shift_up.cy(), w, h, 1.2));
  assert(tc.on_finger_down(5, lay.btn_rewind.cx(), lay.btn_rewind.cy(), w, h, 1.2));
  assert(tc.on_finger_down(6, lay.btn_camera.cx(), lay.btn_camera.cy(), w, h, 1.2));
  assert(tc.on_finger_down(7, lay.btn_map.cx(), lay.btn_map.cy(), w, h, 1.2));
  assert(tc.on_finger_down(8, lay.btn_pause.cx(), lay.btn_pause.cy(), w, h, 1.2));
  assert(tc.on_finger_down(9, lay.btn_radio.cx(), lay.btn_radio.cy(), w, h, 1.2));

  assert(tc.output().handbrake);
  const auto act = tc.consume_actions();
  assert(act.shift_up && act.rewind && act.camera && act.map && act.pause && act.radio_next);
  // Consumed actions are cleared on second read.
  const auto act2 = tc.consume_actions();
  assert(!act2.shift_up && !act2.rewind && !act2.pause);

  // Lift fingers -> neutral output.
  for (int id = 1; id <= 9; ++id) tc.on_finger_up(id, 2.0);
  out = tc.output();
  assert(out.steer == 0.0 && out.throttle == 0.0 && !out.handbrake);

  // Auto-hide after 20 s idle, and auto-hide immediately on gamepad input.
  assert(tc.visible(15.0));
  assert(!tc.visible(23.0));
  assert(tc.on_finger_down(10, lay.throttle.cx(), lay.throttle.cy(), w, h, 25.0));
  assert(tc.visible(25.0));
  tc.notify_gamepad_input(25.5);
  assert(!tc.visible(25.6));

  std::printf("  [ok] multi-touch buttons, pedals, actions & gamepad/timeout auto-hide\n");
}

void test_touch_thumbstick_tilt_and_overlay() {
  carview::TouchControls tc;
  const float w = 1280.0f, h = 720.0f;

  // Virtual floating thumbstick
  tc.set_steer_mode(carview::TouchSteerMode::kThumbstick);
  assert(tc.on_finger_down(1, 0.18f, 0.72f, w, h, 1.0));
  assert(tc.on_finger_motion(1, 0.23f, 0.72f, w, h, 1.05));
  assert(tc.output().steer > 0.45 && tc.output().steer < 0.65);
  assert(tc.on_finger_up(1, 1.1));
  assert(std::fabs(tc.output().steer) < 1e-6);

  // Tilt steering mode
  tc.set_steer_mode(carview::TouchSteerMode::kTilt);
  tc.set_tilt_roll(0.02f);  // inside deadzone
  assert(std::fabs(tc.output().steer) < 1e-6);
  tc.set_tilt_roll(-0.35f);
  assert(tc.output().steer < -0.5);

  // Overlay draws shapes into ui::List
  carview::ui::List list;
  tc.set_visibility(carview::TouchVisibility::kAlwaysOn);
  tc.build_overlay(list, w, h, 2.0, true);
  assert(!list.empty());

  std::printf("  [ok] virtual thumbstick, tilt steering & HUD overlay render\n");
}

void test_android_presets_and_turnip() {
  carview::GraphicsPresetSpec ps;
  assert(carview::resolve_graphics_preset("smooth-60", ps));
  assert(ps.msaa == 2 && ps.shadows == "off" && ps.target_fps == 60);

  assert(carview::resolve_graphics_preset("quality-30", ps));
  assert(ps.msaa == 4 && ps.shadows == "low" && ps.target_fps == 30);

  assert(carview::resolve_graphics_preset("low-power", ps));
  assert(ps.msaa == 1 && !ps.bloom && ps.target_fps == 30);

  // Turnip driver auto-discovery under drivers/
  const auto root = std::filesystem::temp_directory_path() / "shift_turnip_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root / "turnip-gen8-v37");
  {
    std::ofstream f(root / "turnip-gen8-v37" / "libvulkan_freedreno.so");
    f << "so";
  }
  const auto auto_adreno = carview::resolve_android_gpu_driver("auto", root, "Adreno 740");
  assert(auto_adreno.using_custom_driver);
  assert(auto_adreno.driver_so.filename() == "libvulkan_freedreno.so");

  const auto sys_only = carview::resolve_android_gpu_driver("system", root, "Adreno 740");
  assert(!sys_only.using_custom_driver);

  const auto mali_auto = carview::resolve_android_gpu_driver("auto", root, "Mali-G710");
  assert(!mali_auto.using_custom_driver);

  // Vulkan 1.1 capability check
  const auto cap = carview::evaluate_vulkan_capabilities("Adreno 740 (Turnip)", (1u << 22) | (1u << 12), true, true, false);
  assert(cap.has_vulkan_1_1 && !cap.has_vulkan_1_3 && cap.eligible);
  assert(cap.report_line.find("VULKAN_CAPABILITY_REPORT") != std::string::npos);

  std::filesystem::remove_all(root, ec);
  std::printf("  [ok] Android presets (smooth-60/quality-30/low-power), Turnip hook & Vulkan 1.1 cap check\n");
}

void test_macos_shortcuts_and_portable() {
  using carview::PlatformShortcut;
  // On macOS, Cmd shortcuts match cleanly
  assert(carview::match_macos_shortcut(SDLK_COMMA, SDL_KMOD_LGUI, true) == PlatformShortcut::kSettings);
  assert(carview::match_macos_shortcut(SDLK_F, SDL_KMOD_LGUI | SDL_KMOD_LCTRL, true) == PlatformShortcut::kToggleFullscreen);
  assert(carview::match_macos_shortcut(SDLK_P, SDL_KMOD_LGUI | SDL_KMOD_LSHIFT, true) == PlatformShortcut::kPhotoMode);
  assert(carview::match_macos_shortcut(SDLK_T, SDL_KMOD_RGUI, true) == PlatformShortcut::kTrainerOrTime);
  assert(carview::match_macos_shortcut(SDLK_Q, SDL_KMOD_LGUI, true) == PlatformShortcut::kQuit);
  // On non-macOS, Cmd/Win shortcuts do not fire
  assert(carview::match_macos_shortcut(SDLK_Q, SDL_KMOD_LGUI, false) == PlatformShortcut::kNone);

  // Portable mode resolution (standard + macOS .app bundle parent)
  const auto root = std::filesystem::temp_directory_path() / "shift_carview_portable_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  const auto mac_bin = root / "Shift Engine.app" / "Contents" / "MacOS";
  std::filesystem::create_directories(mac_bin);
  assert(carview::resolve_portable_data_root(mac_bin, true).empty());
  {
    std::ofstream f(root / "portable.txt");
    f << "1\n";
  }
  assert(carview::resolve_portable_data_root(mac_bin, true) == root / "data");
  std::filesystem::remove_all(root, ec);

  std::printf("  [ok] macOS Cmd shortcuts & portable-path resolution\n");
}

void test_android_lifecycle_and_world_polish() {
  // P0-8: Android WILL/DID_ENTER_BACKGROUND, FOREGROUND, LOW_MEMORY lifecycle state machine.
  carview::AppLifecycleState lc;
  assert(!lc.backgrounded);

  auto a1 = lc.handle_event(SDL_EVENT_WILL_ENTER_BACKGROUND, /*audio_was_running=*/true);
  assert(a1.handled && lc.backgrounded && lc.background_count == 1);
  assert(a1.pause_audio && a1.flush_save && a1.wait_gpu_idle && !a1.resume_audio);

  auto a2 = lc.handle_event(SDL_EVENT_DID_ENTER_BACKGROUND, /*audio_was_running=*/false);
  assert(a2.handled && lc.backgrounded && !a2.pause_audio);

  auto a3 = lc.handle_event(SDL_EVENT_LOW_MEMORY, /*audio_was_running=*/false);
  assert(a3.handled && a3.trim_memory && lc.low_memory_count == 1);

  auto a4 = lc.handle_event(SDL_EVENT_WILL_ENTER_FOREGROUND, /*audio_was_running=*/false);
  assert(a4.handled && a4.recreate_swapchain);

  auto a5 = lc.handle_event(SDL_EVENT_DID_ENTER_FOREGROUND, /*audio_was_running=*/false);
  assert(a5.handled && !lc.backgrounded && lc.foreground_count == 1);
  assert(a5.recreate_swapchain && a5.resume_audio && a5.reset_frame_clock);

  // Touch normalized -> live window/pixel coordinates helper.
  const auto pt = carview::touch_norm_to_coords(0.5f, 0.25f, 1920, 1080);
  assert(std::fabs(pt.x - 960.0f) < 0.01f && std::fabs(pt.y - 270.0f) < 0.01f);

  // P1: Smashed fence batch > 12 m is eligible for hiding; buildings/roads/terrain/sky are protected.
  assert(carview::Showroom::is_smashable_prop_draw(1u, 0u, 3.0f, 2.0f, 1.0f));      // small sign
  assert(carview::Showroom::is_smashable_prop_draw(1u, 0u, 36.0f, 2.2f, 0.8f));     // 36 m roadside fence batch!
  assert(!carview::Showroom::is_smashable_prop_draw(1u, 0u, 22.0f, 14.0f, 18.0f));  // tall building protected
  assert(!carview::Showroom::is_smashable_prop_draw(2u, 0u, 5.0f, 2.0f, 5.0f));     // sky protected
  assert(!carview::Showroom::is_smashable_prop_draw(1048576u, 0u, 10.0f, 1.0f, 10.0f));  // road protected
  assert(!carview::Showroom::is_smashable_prop_draw(8u, 0u, 10.0f, 1.0f, 10.0f));        // splat terrain protected
  assert(!carview::Showroom::is_smashable_prop_draw(1u, 1u, 6.0f, 0.1f, 6.0f));          // decal protected

  // P1: Showroom::Instance defaults fade_alpha = 1.0f and stores smooth fade values.
  carview::Showroom::Instance inst{};
  assert(inst.fade_alpha == 1.0f);
  inst.fade_alpha = 0.35f;
  assert(std::fabs(inst.fade_alpha - 0.35f) < 1e-6f);

  std::printf("  [ok] Android lifecycle, touch coords, >12m fence hiding & smooth traffic fade\n");
}

}  // namespace

int main() {
  std::printf("platform_touch_tests:\n");
  test_touch_layout_aspects();
  test_touch_multitouch_buttons_and_pedals();
  test_touch_thumbstick_tilt_and_overlay();
  test_android_presets_and_turnip();
  test_macos_shortcuts_and_portable();
  test_android_lifecycle_and_world_polish();
  std::printf("platform_touch_tests: all passed.\n");
  return 0;
}
