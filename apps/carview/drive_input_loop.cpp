#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "display_hdr_win.h"
#include "settings.h"

namespace carview {

namespace {
// Latch preventing gamepad X from leaking into Layout 3 Brake while held after
// activating a world prompt (Festival/Outpost/Event/Hub/H2H).
bool g_x_prompt_consumed = false;
}  // namespace

void DriveSession::try_trigger_rewind() {
              drive::RewindCameraState cs;
              if (have_rig) cs.rig = rig;
              cs.view = last_cam_view;
              cs.has_view = have_cam_view;
              cs.front_view = app.front;
              const double rc = app.race ? app.race->session().clock() : vehicle.time();
              const auto sh = app.skill_tracker ? app.skill_tracker->hud() : freeroam::SkillHudState{};
              const double sk_timer = app.skill_tracker ? app.skill_tracker->chain_remaining_s(vehicle.time()) : 0.0;
              if (rewind_buf.trigger(vehicle.time(), rc, vehicle, cs, uint64_t(std::max(0, sh.chain_points)),
                                     std::max(1, sh.multiplier), sk_timer)) {
                capture_world_rewind(rewind_buf.cancel_snapshot_id());
              } else {
                if (!rewind_buf.enabled()) notice = "rewind is off", notice_until = vehicle.time() + 2;
              }
}

void DriveSession::poll_sdl_events() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
            if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.key == SDLK_AC_BACK) {
              e.key.key = SDLK_ESCAPE;
            }
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) running = false;
            // Android / mobile lifecycle (WILL/DID_ENTER_BACKGROUND, FOREGROUND, LOW_MEMORY).
            if (const auto lc = app_lifecycle.handle_event(e.type, app.audio_host.running()); lc.handled) {
              if (lc.pause_audio) app.audio_host.stop_device();
              if (lc.flush_save) save_now("background");
              if (lc.wait_gpu_idle) host.renderer.wait_idle();
              if (lc.recreate_swapchain) host.renderer.invalidate_swapchain();
              if (lc.resume_audio) app.audio_host.start_device();
              if (lc.reset_frame_clock) {
                window_minimized = false;
                window_occluded = false;
                last_tick_ns = SDL_GetTicksNS();
                host.frame_pacer.reset();
              }
              if (lc.trim_memory) {
                host.showroom.set_low_memory_mode(true);
                host.showroom.set_world_texture_budget({48u * 1024u * 1024u, 256u});
                if (!festival_guests.empty()) {
                  host.renderer.wait_idle();
                  host.showroom.set_instances({});
                  festival_guests.clear();
                }
              }
              continue;
            }
            // Instant alt-tab / focus / occlusion fast path (handled before any overlay
            // continue guards; ZERO wait_idle and ZERO swapchain recreate on focus gain).
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
              host.renderer.set_window_focused(false);
              gamepad_rumble.reset();
              for (SDL_Gamepad* pad : pads) {
                SDL_RumbleGamepad(pad, 0, 0, 0);
                SDL_RumbleGamepadTriggers(pad, 0, 0, 0);
              }
              slice12.on_sdl_event(e, pads);
            } else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
              window_minimized = false;
              window_occluded = false;
              host.renderer.set_window_focused(true);
              slice12.on_sdl_event(e, pads);
              last_tick_ns = SDL_GetTicksNS();
              host.frame_pacer.reset();
            } else if (e.type == SDL_EVENT_WINDOW_MINIMIZED) {
              window_minimized = true;
              gamepad_rumble.reset();
              for (SDL_Gamepad* pad : pads) {
                SDL_RumbleGamepad(pad, 0, 0, 0);
                SDL_RumbleGamepadTriggers(pad, 0, 0, 0);
              }
            } else if (e.type == SDL_EVENT_WINDOW_RESTORED || e.type == SDL_EVENT_WINDOW_EXPOSED) {
              window_minimized = false;
              window_occluded = false;
              last_tick_ns = SDL_GetTicksNS();
              host.frame_pacer.reset();
            } else if (e.type == SDL_EVENT_WINDOW_OCCLUDED) {
              window_occluded = true;
            }
            // Universal controller hot-plug (works on title screen, every menu, and driving).
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
              bool dup = false;
              for (SDL_Gamepad* p : pads)
                if (SDL_GetGamepadID(p) == e.gdevice.which) dup = true;
              if (!dup) {
                if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) {
                  pads.push_back(pad);
                  std::printf("gamepad: %s\n", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "(unnamed)");
                  slice12.sync_pad_capabilities(pads, true);
                  slice12.sync_wheel_devices();
                }
              }
              continue;
            }
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
              for (size_t k = 0; k < pads.size(); ++k)
                if (SDL_GetGamepadID(pads[k]) == e.gdevice.which) {
                  SDL_CloseGamepad(pads[k]);
                  pads.erase(pads.begin() + long(k));
                  if (pads.empty()) g_x_prompt_consumed = false;
                  slice12.sync_pad_capabilities(pads, false);
                  slice12.sync_wheel_devices();
                  break;
                }
              continue;
            }
            if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED) {
              slice12.sync_wheel_devices();
              continue;
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) {
              g_x_prompt_consumed = false;
            }
            // R3-UI-02: Auto-switch UI4 GlyphSet (Keyboard / Xbox / PlayStation) by most recent active input device.
            if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                e.type == SDL_EVENT_MOUSE_WHEEL) {
              slice12.notify_keyboard_or_mouse_input();
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
              slice12.notify_gamepad_input(e.gbutton.which, pads);
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(e.gaxis.value)) > 10000) {
              slice12.notify_gamepad_input(e.gaxis.which, pads);
            } else if ((e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || e.type == SDL_EVENT_JOYSTICK_HAT_MOTION) &&
                       !SDL_IsGamepad(e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? e.jbutton.which : e.jhat.which)) {
              slice12.notify_gamepad_input(0, pads);
            }
            // macOS Command shortcuts (Cmd+,, Ctrl+Cmd+F, Cmd+Shift+P, Cmd+T, Cmd+Q).
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
#if defined(__APPLE__)
              constexpr bool kIsMacOs = true;
#else
              constexpr bool kIsMacOs = false;
#endif
              const auto sc = carview::match_macos_shortcut(e.key.key, e.key.mod, kIsMacOs);
              if (sc == carview::PlatformShortcut::kQuit) {
                running = false;
                continue;
              } else if (sc == carview::PlatformShortcut::kSettings) {
                settings_page.load(read_settings(settings_path(), nullptr));
                settings_page.open();
                options_page = carview::OptionsPage::kNone;  // the engine's page, not an option page
                settings_live_overlay = true;
                app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
                continue;
              } else if (sc == carview::PlatformShortcut::kToggleFullscreen) {
                const bool fs = (SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(app.window, !fs);
                continue;
              } else if (sc == carview::PlatformShortcut::kPhotoMode && !picking && !(app.race && app.race->player_finished())) {
                const drive::Vec3 pat = vehicle.position();
                const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                photo_mode.set_metric_units(app.metric_units);
                photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                                 carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
                continue;
              } else if (sc == carview::PlatformShortcut::kTrainerOrTime) {
                app.tod_clock.step_hours(1.0);
                if (traffic) traffic->set_hour(app.tod_clock.hour());
                notice = "MACOS CMD+T: STEPPED TIME +1H";
                notice_until = vehicle.time() + 2.5;
                continue;
              }
            }
            // Android / touchscreen multi-touch driving controls & menu tap routing (P0-8).
            if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
                e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
              int live_px_w = app.width, live_px_h = app.height;
              int live_win_w = app.width, live_win_h = app.height;
              if (app.window) {
                SDL_GetWindowSizeInPixels(app.window, &live_px_w, &live_px_h);
                SDL_GetWindowSize(app.window, &live_win_w, &live_win_h);
              }
              const bool touch_menu_open =
                  app.show_title || app.fmv_player.active() || ceremony.active || grid_menu_open ||
                  menu.open || map_open || picking || events_page.is_open() || outpost_hub_open || garage_open ||
                  shop.is_open() || autoshow.is_open() || career_page.is_open() || hub_menu.open() || settings_page.is_open() ||
                  paint_open || club_open || car_club_open || app.rally_hub_open || app.rally_service_open || app.rally_results_open ||
                  photo_mode.active() || replay_viewer.active() || replay_viewer.browser_open() ||
                  achievements.screen_open() || mods_page.is_open() || save_page.is_open() ||
                  slice12.trainer_overlay.is_open() || rewind_buf.active() || (app.race && app.race->player_finished());
              if (touch_menu_open && e.type != SDL_EVENT_FINGER_CANCELED) {
                const auto pt = carview::touch_norm_to_coords(e.tfinger.x, e.tfinger.y,
                                                              live_win_w > 0 ? live_win_w : live_px_w,
                                                              live_win_h > 0 ? live_win_h : live_px_h);
                if (e.type == SDL_EVENT_FINGER_DOWN) {
                  e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                  e.button.button = SDL_BUTTON_LEFT;
                  e.button.down = true;
                  e.button.clicks = 1;
                  e.button.x = pt.x;
                  e.button.y = pt.y;
                } else if (e.type == SDL_EVENT_FINGER_MOTION) {
                  e.type = SDL_EVENT_MOUSE_MOTION;
                  e.motion.x = pt.x;
                  e.motion.y = pt.y;
                } else if (e.type == SDL_EVENT_FINGER_UP) {
                  e.type = SDL_EVENT_MOUSE_BUTTON_UP;
                  e.button.button = SDL_BUTTON_LEFT;
                  e.button.down = false;
                  e.button.x = pt.x;
                  e.button.y = pt.y;
                }
              } else {
                app.touch_controls.handle_sdl_event(e, float(std::max(1, live_px_w)), float(std::max(1, live_px_h)),
                                                vehicle.time());
              }
            }
    if (handle_campaign_and_title_event(e)) continue;
    if (handle_grid_and_map_event(e)) continue;
    if (handle_events_and_outpost_event(e)) continue;
    if (handle_hub_shop_event(e)) continue;
    if (handle_rally_career_club_event(e)) continue;
    if (handle_dev_overlays_event(e)) continue;
    if (handle_settings_and_media_event(e)) continue;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
              const bool on_banner = app.race && app.race->player_finished() && !picking && results_page == 0;
              if (e.key.key == SDLK_ESCAPE) {
                if (on_banner && results_table) results_table = false;  // B: back to the banner
                else if (app.race && race_screens.screen() == race_flow::Screen::kStayInCar) race_back();  // B: NO
                else if (picking) picking = false;
                else if (!app.race && app.pr_stunt_session.active()) {
                  app.pr_stunt_session.cancel();
                  app.gps_nav.clear();
                  notice = "PR STUNT CANCELLED";
                  notice_until = vehicle.time() + 2.5;
                } else open_menu();
              }
              const std::string kn = sdl_key_name(e.key.key);
              const bool photo_key =
                  (e.key.key == SDLK_P) || live_controls.keys.matches(drive::ControlAction::kPhotoMode, kn);
              if (!app.race && app.pr_stunt_session.active() && photo_key) {
                pr_stunt_photo_pressed = true;
              } else if (photo_key && !picking && !(app.race && app.race->player_finished())) {
                const drive::Vec3 pat = vehicle.position();
                const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                photo_mode.set_metric_units(app.metric_units);
                photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                                 carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
              }
              if (e.key.key != SDLK_F3 && live_controls.keys.matches(drive::ControlAction::kTelemetry, kn)) {
                host.perf_overlay_open = !host.perf_overlay_open;
                host.renderer.set_gpu_timing(host.perf_overlay_open || host.inspector_open);
                notice = host.perf_overlay_open ? "TELEMETRY OVERLAY: ON" : "TELEMETRY OVERLAY: OFF";
                notice_until = vehicle.time() + 2.0;
              }
              if (on_banner && e.key.key == SDLK_BACKSPACE) results_table = false;
              else if (!on_banner && e.key.key == SDLK_BACKSPACE) try_trigger_rewind();
              if (on_banner && e.key.key == SDLK_X) results_table = true;  // X: RACE RESULTS
              if (on_banner && e.key.key == SDLK_V) open_replay_viewer(false);  // V: VIEW REPLAY
              if (!app.race && live_controls.keys.matches(drive::ControlAction::kActivate, kn)) activate_world_trigger();
              if (live_controls.keys.matches(drive::ControlAction::kReset, kn)) put_back();
              if (!on_banner && live_controls.keys.matches(drive::ControlAction::kRewind, kn)) try_trigger_rewind();
              if (live_controls.keys.matches(drive::ControlAction::kShiftUp, kn)) pending_shift = +1;
              if (live_controls.keys.matches(drive::ControlAction::kShiftDown, kn)) pending_shift = -1;
              if (e.key.key == SDLK_L) {
                if (e.key.mod & SDL_KMOD_SHIFT) {
                  light_override = (light_override + 1) % 4;
                  const char* lnames[4] = {"lights: auto", "lights: low beam", "lights: full beam", "lights: fog"};
                  notice = lnames[light_override];
                  notice_until = vehicle.time() + 2;
                } else {
                  driving_line_mode =
                      static_cast<drive::DrivingLineMode>((static_cast<int>(driving_line_mode) + 1) % 3);
                  live_controls.assists.driving_line = static_cast<int>(driving_line_mode);
                  const char* names[3] = {"driving line: off", "driving line: braking only", "driving line: full"};
                  notice = names[static_cast<int>(driving_line_mode)];
                  notice_until = vehicle.time() + 2;
                }
              }
              if (live_controls.keys.matches(drive::ControlAction::kMap, kn)) open_map();
              if (live_controls.keys.matches(drive::ControlAction::kRadioPrev, kn)) radio_press(-1);
              if (live_controls.keys.matches(drive::ControlAction::kRadioNext, kn)) radio_press(+1);
              if (app.audio_host.handle_key(e.key.key, e.key.mod, notice)) notice_until = vehicle.time() + 3;
              // SwitchCamera (the rig); without the rig, the front view.
              if (live_controls.keys.matches(drive::ControlAction::kSwitchCamera, kn)) {
                if (have_rig) rig.cycle();
                else app.front = !app.front;
              }
              if (e.key.key == SDLK_LEFTBRACKET) browse(-1);
              if (e.key.key == SDLK_RIGHTBRACKET) browse(1);
              const bool results = app.race && app.race->player_finished() && !picking;
              if (results && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) race_continue();
              else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                if (picking) take_pick();
                else activate_world_trigger();
              }
              if (results && e.key.key == SDLK_Y) race_restart();
              else if (!results && e.key.key == SDLK_Y) try_trigger_rewind();
              if (e.key.key == SDLK_T) {
                const bool back = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                app.tod_clock.step_hours(back ? -1.0 : 1.0);
                if (traffic) traffic->set_hour(app.tod_clock.hour());
                const int tot_min = int(app.tod_clock.tod_sec / 60.0);
                char line_buf[64];
                std::snprintf(line_buf, sizeof line_buf, "TIME %02d:%02d", (tot_min / 60) % 24, tot_min % 60);
                notice = line_buf;
                notice_until = vehicle.time() + 3;
              }
              // F6: live tabbed FH1/Pinyon Shift Settings overlay (Shift+F6 opens live Camera Editor).
              if (e.key.key == SDLK_F6) {
                if (e.key.mod & SDL_KMOD_SHIFT) {
                  slice12.cam_editor_overlay.open(slice12.cam_editor_cfg, slice12.imm_cam_cfg, slice12.ht_cfg);
                } else {
                  settings_page.load(read_settings(settings_path(), nullptr));
                  settings_page.open();
                  options_page = carview::OptionsPage::kNone;  // the engine's page, not an option page
                  settings_live_overlay = true;
                }
                app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
              }
              // F10: Pinyon Shift Trainer overlay (isolated user-modded save).
              if (e.key.key == SDLK_F10) {
                slice12.trainer_overlay.toggle();
                app.audio_host.trigger_ui(slice12.trainer_overlay.is_open() ? audio::UiCue::kMenuOpen : audio::UiCue::kCancel);
              }
              // F5: reload the mods' replacement textures (texture_mods.h).
              if (e.key.key == SDLK_F5 && !e.key.repeat) reload_mod_textures();
              // F7: Pinyon Shift-style Achievements screen.
              if (e.key.key == SDLK_F7) {
                eval_achievements(false);
                achievements.open_screen();
                app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
              }
              // F8: real Vulkan swapchain PNG capture to <user-data>/photos/.
              if (e.key.key == SDLK_F8) {
                pending_shot = PendingShot::kF8;
                host.renderer.request_capture(show_hud);
              }
              // F12: pose report (carview-pose-N.txt) + companion PNG in photos/.
              if (e.key.key == SDLK_F12) {
                                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                char name[64], line_buf[96];
                std::snprintf(name, sizeof name, "carview-pose-%d.txt", ++app.pose_shot_counter);
                if (FILE* pf = std::fopen(name, "w")) {
                  std::fprintf(pf, "car %s\nposition %.2f %.2f %.2f\nheading %.1f deg\nspeed %.1f km/h\n"
                               "time %.2f s\nhour %s\ncamera %s\nspawn %.2f %.2f %.2f %.4f\n",
                               app.car.c_str(), at.x, at.y, at.z, std::atan2(f.x, f.z) * 57.29578, vehicle.speed() * 3.6,
                               vehicle.time(), traffic ? std::to_string(traffic->hour()).c_str() : "12 (no traffic)",
                               have_rig ? drive::camera_mode_name(rig.mode()) : (app.front ? "front" : "chase"), at.x, at.y, at.z,
                               std::atan2(f.x, f.z));
                  std::fclose(pf);
                  std::snprintf(line_buf, sizeof line_buf, "pose + png saved: %s", name);
                  pending_f12_pose_name = name;
                  pending_shot = PendingShot::kF12;
                  host.renderer.request_capture(show_hud);
                } else {
                  std::snprintf(line_buf, sizeof line_buf, "cannot write %s", name);
                }
                notice = line_buf;
                notice_until = vehicle.time() + 3;
              }
              if (e.key.key == SDLK_H || e.key.key == SDLK_F2) {
                const bool h2h_prompt =
                    e.key.key == SDLK_H && festival_mgr && !app.race && festival_mgr->hud(vehicle.time()).prompt_visible;
                if (h2h_prompt) {
                  const drive::Vec3 fp = vehicle.position();
                  const drive::Vec3 ff = drive::rotate(vehicle.orientation(), {0, 0, 1});
                  if (festival_mgr->start_head_to_head({fp.x, fp.y, fp.z}, std::atan2(ff.x, ff.z), vehicle.time())) {
                    club_pending.encounter_started = true;
                    app.audio_host.trigger_ui(audio::UiCue::kH2hGo);
                    notice = "HEAD-TO-HEAD RACE STARTED!";
                    notice_until = vehicle.time() + 2.5;
                    minimap_at = -1;
                  }
                } else {
                  show_hud = !show_hud;
                  if (!show_hud) {
                    for (int layer : {0, 1, 3}) host.renderer.set_overlay({}, 0, 0, 0, 0, layer);
                    host.renderer.set_ui({});
                    mm_size = 0;
                  }
                  radio_popup_gpu = {};
                  minimap_at = -1;
                }
              }
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
              const drive::ControllerLayout pad_layout = drive::apply_handbrake_clutch_swap(
                  drive::default_controller_layout(live_controls.layout_id), live_controls.switch_handbrake_clutch);
              const drive::PadControl pc = pad_control_from_button(e.gbutton.button);
              const std::string_view act = pad_layout.action_for(pc);
              // R3-UI-02: Consume X completely when activating a world prompt (Festival/Outpost/Event/Hub/H2H/picker)
              // so it never leaks into ShiftDown / ShiftUp / Rewind / Brake on any controller layout.
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST && !(app.race && app.race->player_finished())) {
                const bool has_prompt =
                    picking || active_trigger.in_range ||
                    (festival_mgr && !app.race && festival_mgr->hud(vehicle.time()).prompt_visible);
                if (has_prompt) {
                  if (picking) take_pick();
                  else activate_world_trigger();
                  g_x_prompt_consumed = true;
                  continue;
                }
              }
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) open_menu();
              if (act == "Rewind" && !(app.race && app.race->player_finished()) && !picking) {
                try_trigger_rewind();
              } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                if (rewind_buf.rewind_on_back() && !(app.race && app.race->player_finished()) && !picking) try_trigger_rewind();
                else open_map();
              } else if (act == "Map") {
                open_map();
              }
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH && act != "Rewind" && act != "SwitchCamera" &&
                  !rewind_buf.rewind_on_back() && !(app.race && app.race->player_finished()) && !picking)
                try_trigger_rewind();
              if (act == "PhotoMode" && !picking && !(app.race && app.race->player_finished())) {
                if (!app.race && app.pr_stunt_session.active()) {
                  pr_stunt_photo_pressed = true;
                } else {
                  const drive::Vec3 pat = vehicle.position();
                  const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                  photo_mode.set_metric_units(app.metric_units);
                  photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                                   carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
                }
              }
              if (act == "Telemetry" && !picking && !(app.race && app.race->player_finished())) {
                host.perf_overlay_open = !host.perf_overlay_open;
                host.renderer.set_gpu_timing(host.perf_overlay_open || host.inspector_open);
                notice = host.perf_overlay_open ? "TELEMETRY OVERLAY: ON" : "TELEMETRY OVERLAY: OFF";
                notice_until = vehicle.time() + 2.0;
              }
              if (act == "RadioLeft") radio_press(-1);
              if (act == "RadioRight") radio_press(+1);
              if (act == "ShiftUp") pending_shift = +1;
              if (act == "ShiftDown") pending_shift = -1;
              if (act == "SwitchCamera") {
                if (picking || !have_rig) browse(1);
                else rig.cycle();
              }
              if (picking && e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) browse(-1);
              if (picking && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER && act != "SwitchCamera") browse(1);
              if (app.race && app.race->player_finished() && !picking) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) race_continue();
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) race_restart();
                // X opens the RACE RESULTS table from the banner; B closes it or opens Replay (SPEC-EVENT-REWARDS).
                if (results_page == 0 && e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) results_table = true;
                if (results_page == 0 && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) open_replay_viewer(false);
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                  if (results_table) results_table = false;
                  else if (results_page == 0) open_replay_viewer(false);
                  else race_back();
                }
              }
            }
            if (app.race && app.race->player_finished() && !picking && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
              if (e.button.button == SDL_BUTTON_RIGHT) {
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
                if (results_table) results_table = false;
                else race_continue();
              } else if (e.button.button == SDL_BUTTON_LEFT) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui4::kHitButton) {
                  if (results_page == 0 && results_table) {
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                    results_table = false;
                  } else if (results_page == 0) {
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    if (hz.index == 0) race_continue();
                    else if (hz.index == 1) race_restart();
                    else if (hz.index == 2) results_table = true;
                    else if (hz.index == 3) open_replay_viewer(false);
                  } else {
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    race_continue();
                  }
                } else {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  if (results_table) results_table = false;
                  else race_continue();
                }
              }
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
              if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) {
                pads.push_back(pad);
                std::printf("gamepad: %s\n", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "(unnamed)");
                slice12.sync_pad_capabilities(pads);
                slice12.sync_wheel_devices();
              }
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
              for (size_t k = 0; k < pads.size(); ++k)
                if (SDL_GetGamepadID(pads[k]) == e.gdevice.which) {
                  SDL_CloseGamepad(pads[k]);
                  pads.erase(pads.begin() + long(k));
                  slice12.sync_pad_capabilities(pads);
                  slice12.sync_wheel_devices();
                  break;
                }
            if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED) {
              slice12.sync_wheel_devices();
            }
  }
  // R3-UI-01: Left-stick menu navigation with deadzone, axis-dominance lock & repeat
  // routed through the FULL modal input chain across all core screens (excluding continuous
  // analog left-stick modes: World Map pan, Photo Mode camera flight, and Livery vinyl drag).
  static MenuStickNav menu_stick_nav;
  static bool menu_stick_was_active = false;
  const bool livery_non_vinyl_open =
      paint_open && (!livery_editor.is_open() || livery_editor.catalog_open() ||
                     livery_editor.tab() != carview::EditorTab::kVinylLayers);
  const bool title_nav_open = app.show_title && !settings_page.is_open() && app.title.menu_visible();
  const bool blocking_video_or_map =
      app.fmv_player.active() || loading_screen.active() || map_open ||
      (photo_mode.active() && !photo_mode.gallery_open()) || rewind_buf.active();
  const bool menu_nav_active =
      !pads.empty() && !blocking_video_or_map &&
      (title_nav_open || (app.race && grid_menu_open) || events_page.is_open() || outpost_hub_open || garage_open ||
       autoshow.is_open() || shop.is_open() || livery_non_vinyl_open || club_open || car_club_open ||
       app.rally_hub_open || app.rally_service_open || app.rally_results_open || replay_viewer.browser_open() ||
       (photo_mode.active() && photo_mode.gallery_open()) ||
       (settings_page.is_open() && !settings_page.capturing_key()) || menu.open || career_page.is_open() || hub_menu.open() ||
       achievements.screen_open() || mods_page.is_open() || save_page.is_open() ||
       slice12.trainer_overlay.is_open() || slice12.cam_editor_overlay.is_open());
  if (!menu_nav_active) {
    menu_stick_nav.reset();
    menu_stick_was_active = false;
  } else {
    double best_lx = 0.0, best_ly = 0.0, best_mag2 = -1.0;
    SDL_JoystickID best_which = 0;
    for (SDL_Gamepad* pad : pads) {
      const double lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0;
      const double ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0;
      const double mag2 = lx * lx + ly * ly;
      if (mag2 > best_mag2) {
        best_mag2 = mag2;
        best_lx = lx;
        best_ly = ly;
        best_which = SDL_GetGamepadID(pad);
      }
    }
    if (!menu_stick_was_active) {
      menu_stick_was_active = true;
      if (std::fabs(best_lx) >= menu_stick_nav.release_threshold ||
          std::fabs(best_ly) >= menu_stick_nav.release_threshold) {
        menu_stick_nav.latch_until_neutral();
      }
    }
    const double now_s = double(SDL_GetTicksNS()) * 1e-9;
    const MenuStickNav::Dir dir = menu_stick_nav.update(best_lx, best_ly, now_s);
    if (dir != MenuStickNav::Dir::kNone) {
      slice12.notify_gamepad_input(best_which, pads);
      SDL_Event syn{};
      syn.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      syn.gbutton.which = best_which;
      syn.gbutton.down = true;
      switch (dir) {
        case MenuStickNav::Dir::kUp: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_UP; break;
        case MenuStickNav::Dir::kDown: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_DOWN; break;
        case MenuStickNav::Dir::kLeft: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_LEFT; break;
        case MenuStickNav::Dir::kRight: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_RIGHT; break;
        case MenuStickNav::Dir::kNone: break;
      }
      if (!handle_campaign_and_title_event(syn) &&
          !handle_grid_and_map_event(syn) &&
          !handle_events_and_outpost_event(syn) &&
          !handle_hub_shop_event(syn) &&
          !handle_rally_career_club_event(syn) &&
          !handle_dev_overlays_event(syn)) {
        handle_settings_and_media_event(syn);
      }
    }
  }
}

void DriveSession::sample_driving_input(drive::Input& in, double& rig_look) {
          const bool* keys = SDL_GetKeyboardState(nullptr);
          constexpr double kLookPi = 3.14159265358979323846;
          // Assisted braking looks as far ahead as the driving line brakes (FEEL-14).
          constexpr double kAutoBrakeLookM = drive::DrivingLineOptions{}.brake_lookahead_m;
          drive::Input kb_in;
          kb_in.steer = (action_key_down(keys, live_controls.keys, drive::ControlAction::kSteerRight) ? 1.0 : 0.0) -
                        (action_key_down(keys, live_controls.keys, drive::ControlAction::kSteerLeft) ? 1.0 : 0.0);
          kb_in.throttle = action_key_down(keys, live_controls.keys, drive::ControlAction::kThrottle) ? 1.0 : 0.0;
          kb_in.brake = action_key_down(keys, live_controls.keys, drive::ControlAction::kBrake) ? 1.0 : 0.0;
          kb_in.handbrake = action_key_down(keys, live_controls.keys, drive::ControlAction::kHandbrake);
          kb_in.clutch = action_key_down(keys, live_controls.keys, drive::ControlAction::kClutch) ? 1.0 : 0.0;
          // Look around: keyboard LookBack / LookLeft / LookRight keys, plus the
          // gamepad right stick's LookBack / LookLeft / LookRight snaps (and the
          // stick click's LookStraightBack) in every camera (FEEL-26).
          const bool kb_look_back = action_key_down(keys, live_controls.keys, drive::ControlAction::kLookBack);
          const bool kb_look_left = action_key_down(keys, live_controls.keys, drive::ControlAction::kLookLeft);
          const bool kb_look_right = action_key_down(keys, live_controls.keys, drive::ControlAction::kLookRight);
          double kb_look = 0.0;
          if (kb_look_back) kb_look = kLookPi;
          else if (kb_look_left && !kb_look_right) kb_look = -0.5 * kLookPi;
          else if (kb_look_right && !kb_look_left) kb_look = 0.5 * kLookPi;
          camera_pull.look_target = kb_look_back ? kLookPi : -kb_look;
          rig_look = kb_look;
          const drive::ControllerLayout pad_layout = drive::apply_handbrake_clutch_swap(
              drive::default_controller_layout(live_controls.layout_id), live_controls.switch_handbrake_clutch);
          const auto& dz = live_controls.deadzones;
          const drive::PadControl gas_ctrl = pad_layout.control_for("Gas");
          const drive::PadControl brake_ctrl = pad_layout.control_for("Brake");
          const drive::PadControl hb_ctrl = pad_layout.control_for("Handbrake");
          const drive::PadControl clutch_ctrl = pad_layout.control_for("Clutch");
          drive::Input pad_in;
          bool pad_used = false;
          bool pad_was_wheel = false;
          for (SDL_Gamepad* pad : pads) {
            auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
            const char* pname = SDL_GetGamepadName(pad);
            const bool is_wheel_pad = carview::is_racing_wheel_device(
                SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad), pname ? pname : "");
            // A wheel takes WheelAdvancedOptions' dead zones, a pad
            // ControllerAdvancedOptions' (FEEL-18).
            static const drive::ControlDeadzones kWheelDz = drive::ControlDeadzones::wheel_defaults();
            const drive::ControlDeadzones& pdz = is_wheel_pad ? kWheelDz : dz;
            const double raw_steer = axis(SDL_GAMEPAD_AXIS_LEFTX);
            const double steer =
                is_wheel_pad
                    ? drive::shape_wheel_steering(
                          drive::apply_deadzone_signed(raw_steer, pdz.steering_inside, pdz.steering_outside),
                          slice12.wheel_shaping)
                    : drive::apply_deadzone_signed(raw_steer, pdz.steering_inside, pdz.steering_outside);
            const double raw_throttle =
                (g_x_prompt_consumed && gas_ctrl == drive::PadControl::kButtonX)
                    ? 0.0
                    : pad_control_value(pad, gas_ctrl, pdz.throttle_inside, pdz.throttle_outside);
            const double raw_brake =
                (g_x_prompt_consumed && brake_ctrl == drive::PadControl::kButtonX)
                    ? 0.0
                    : pad_control_value(pad, brake_ctrl, pdz.brake_inside, pdz.brake_outside);
            const double throttle = is_wheel_pad ? drive::shape_wheel_pedal(raw_throttle, slice12.wheel_shaping) : raw_throttle;
            const double brake = is_wheel_pad ? drive::shape_wheel_pedal(raw_brake, slice12.wheel_shaping) : raw_brake;
            const bool handbrake =
                !(g_x_prompt_consumed && hb_ctrl == drive::PadControl::kButtonX) &&
                pad_control_value(pad, hb_ctrl, pdz.handbrake_inside, pdz.handbrake_outside) > 0.5;
            const double clutch =
                (g_x_prompt_consumed && clutch_ctrl == drive::PadControl::kButtonX)
                    ? 0.0
                    : pad_control_value(pad, clutch_ctrl, pdz.clutch_inside, pdz.clutch_outside);
            // Right-stick camera look (SPEC-CAMERAS, look-around): the stick picks
            // one of the original's targets, back (pi) or left/right (pi/2), which
            // the look spring reaches; released, it springs forward again. The
            // chase cameras no longer orbit continuously (an engine invention).
            const double rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
              rig_look = kLookPi;
              camera_pull.look_target = kLookPi;
            } else {
              const double snap = drive::right_stick_look_target(rx, ry);
              if (snap != 0.0) {
                rig_look = snap;
                camera_pull.look_target = snap > 0.75 * kLookPi ? kLookPi : -snap;
              }
            }
            if (steer != 0 || throttle > 0 || brake > 0 || handbrake || clutch > 0) {
              pad_in.steer = steer;
              pad_in.throttle = throttle;
              pad_in.brake = brake;
              pad_in.handbrake = handbrake;
              pad_in.clutch = clutch;
              pad_used = true;
              pad_was_wheel = is_wheel_pad;
              slice12.notify_gamepad_input(SDL_GetGamepadID(pad), pads);
              app.touch_controls.notify_gamepad_input(vehicle.time());
              break;
            }
          }
          if (!pad_used &&
              (kb_in.steer != 0 || kb_in.throttle > 0 || kb_in.brake > 0 || kb_in.handbrake || kb_in.clutch > 0)) {
            slice12.notify_keyboard_or_mouse_input();
          }
          const carview::RawJoystickDriveOutput raw_joy = slice12.sample_raw_joystick_input(dz);
          if (!pad_used && raw_joy.any_active) {
            if (raw_joy.look_x != 0.0) camera_pull.look_target = -raw_joy.look_x * kLookPi;
            app.touch_controls.notify_gamepad_input(vehicle.time());
          }
          const carview::RawJoystickDriveOutput wheel_joy = slice12.sample_wheel_input();
          in = carview::merge_driving_inputs(kb_in, pad_in, pad_used, raw_joy.input, raw_joy.any_active,
                                             wheel_joy.input, wheel_joy.any_active);
          if (wheel_joy.any_active || (pad_used && pad_was_wheel)) {
            in.linear_steer = true;
          }
          slice12.apply_gyro_input(in);
          bool touch_steered = false;
          // Apply Android multi-touch controls & edge-triggered HUD actions.
          app.touch_controls.set_gamepad_connected(!pads.empty() || slice12.has_any_raw_controller());
          app.touch_controls.update(vehicle.time());
          {
            const carview::TouchActions ta = app.touch_controls.consume_actions();
            if (ta.pause) open_menu();
            if (ta.map) open_map();
            if (ta.camera) {
              if (have_rig) rig.cycle();
              else app.front = !app.front;
            }
            if (ta.radio_next) radio_press(+1);
            if (ta.shift_up) pending_shift = +1;
            if (ta.shift_down) pending_shift = -1;
            if (ta.rewind) {
              drive::RewindCameraState cs;
              if (have_rig) cs.rig = rig;
              cs.view = last_cam_view;
              cs.has_view = have_cam_view;
              cs.front_view = app.front;
              const double rc = app.race ? app.race->session().clock() : vehicle.time();
              const auto sh = app.skill_tracker ? app.skill_tracker->hud() : freeroam::SkillHudState{};
              const double sk_timer = app.skill_tracker ? app.skill_tracker->chain_remaining_s(vehicle.time()) : 0.0;
              if (rewind_buf.trigger(vehicle.time(), rc, vehicle, cs, uint64_t(std::max(0, sh.chain_points)),
                                     std::max(1, sh.multiplier), sk_timer)) {
                capture_world_rewind(rewind_buf.cancel_snapshot_id());
              }
            }
            const carview::TouchDriveOutput to = app.touch_controls.output();
            if (to.active) {
              in.steer = to.steer;
              in.throttle = std::max(in.throttle, to.throttle);
              in.brake = std::max(in.brake, to.brake);
              in.handbrake = in.handbrake || to.handbrake;
              touch_steered = true;
            }
          }
          in.input_is_keyboard =
              !touch_steered && !in.linear_steer &&
              (std::fabs(kb_in.steer) > 1e-3 || (!pad_used && !raw_joy.any_active && !wheel_joy.any_active));

          // Assisted Steering lane guidance (Steering == 0) & Auto-Brake (Braking == 0)
          // in both active races (racing line) and free roam (active GPS route or road spline).
          if (!app.cruise && !grid_menu_open && !(app.race && app.race->player_finished())) {
            const bool want_lane = (live_controls.assists.steering == 0);
            const bool want_auto_brake = (live_controls.assists.braking == 0);
            if (want_lane || want_auto_brake) {
              const drive::Vec3 car_pos = vehicle.position();
              const drive::Vec3 car_fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
              const double car_hdg = std::atan2(car_fwd.x, car_fwd.z);
              const double car_spd = vehicle.speed();
              const double look_d = std::clamp(std::fabs(car_spd) * 0.35, 4.0, 20.0);

              if (app.race) {
                const double car_prog = app.race->player_progress();
                if (want_lane) {
                  const drive::RacingLine& rline = app.race->racing_line();
                  if (rline.size() >= 2 && rline.length() > 1.0) {
                    double rx = 0.0, ry = 0.0, rz = 0.0, r_hdg = 0.0;
                    rline.pose(car_prog, 0.0, rx, ry, rz, r_hdg);
                    const double lat_off = (car_pos.x - rx) * std::cos(r_hdg) - (car_pos.z - rz) * std::sin(r_hdg);
                    double ax = 0.0, ay = 0.0, az = 0.0, ahead_hdg = r_hdg;
                    rline.pose(std::min(rline.length(), car_prog + look_d), 0.0, ax, ay, az, ahead_hdg);
                    const double hdg_err = std::remainder(ahead_hdg - car_hdg, 2.0 * kLookPi);
                    if (std::fabs(hdg_err) < 1.2 && std::fabs(lat_off) < 25.0) {
                      in.has_assist_lane = true;
                      in.assist_heading_error_rad = hdg_err;
                      in.assist_lateral_offset_m = lat_off;
                    }
                  }
                }
                double v_target = 0.0;
                if (want_auto_brake &&
                    drive::auto_brake_target(app.race->racing_line(), braking_table, car_prog, car_spd, v_target)) {
                  in.has_auto_brake_target = true;
                  in.auto_brake_target_speed = v_target;
                }
              } else {
                // Free-roam lane guidance & curvature lookahead auto-brake:
                // prefer active GPS route when on-route, otherwise nearest road spline.
                bool guided = false;
                if (app.gps_nav.active() && app.gps_nav.route().valid && app.gps_nav.route().points.size() >= 2 &&
                    app.gps_nav.route().s.size() == app.gps_nav.route().points.size() &&
                    app.gps_nav.guidance().off_route_m < 25.0) {
                  const auto& route = app.gps_nav.route();
                  auto sample_gps = [&](double sq, double& px, double& pz, double& tx, double& tz) {
                    sq = std::clamp(sq, 0.0, route.total_length);
                    auto it = std::upper_bound(route.s.begin(), route.s.end(), sq);
                    size_t idx = (it == route.s.begin()) ? 0 : size_t((it - route.s.begin()) - 1);
                    if (idx + 1 >= route.points.size()) idx = route.points.size() - 2;
                    const double s0 = route.s[idx], s1 = route.s[idx + 1];
                    const double frac = (s1 > s0 + 1e-6) ? std::clamp((sq - s0) / (s1 - s0), 0.0, 1.0) : 0.0;
                    const auto& p0 = route.points[idx];
                    const auto& p1 = route.points[idx + 1];
                    px = p0.x + (p1.x - p0.x) * frac;
                    pz = p0.z + (p1.z - p0.z) * frac;
                    const double dx = p1.x - p0.x, dz = p1.z - p0.z;
                    const double len = std::hypot(dx, dz);
                    if (len > 1e-6) {
                      tx = dx / len;
                      tz = dz / len;
                    } else {
                      tx = 0.0;
                      tz = 1.0;
                    }
                  };
                  const double s_cur = app.gps_nav.guidance().route_progress_m;
                  double rx = 0.0, rz = 0.0, tx0 = 0.0, tz0 = 1.0;
                  sample_gps(s_cur, rx, rz, tx0, tz0);
                  double ax = 0.0, az = 0.0, tx1 = tx0, tz1 = tz0;
                  sample_gps(s_cur + look_d, ax, az, tx1, tz1);
                  const double lat_off = (car_pos.x - rx) * tz0 - (car_pos.z - rz) * tx0;
                  const double hdg_err = std::remainder(std::atan2(tx1, tz1) - car_hdg, 2.0 * kLookPi);
                  if (want_lane && std::fabs(hdg_err) < 1.2 && std::fabs(lat_off) < 25.0) {
                    in.has_assist_lane = true;
                    in.assist_heading_error_rad = hdg_err;
                    in.assist_lateral_offset_m = lat_off;
                  }
                  if (want_auto_brake && car_spd > 3.0) {
                    // The route ahead as a path from the car (FEEL-14).
                    std::vector<float> px{float(rx)}, pz{float(rz)};
                    for (double da = 4.0; da <= kAutoBrakeLookM && s_cur + da <= route.total_length; da += 4.0) {
                      double qx = 0.0, qz = 0.0, qtx = 0.0, qtz = 1.0;
                      sample_gps(s_cur + da, qx, qz, qtx, qtz);
                      px.push_back(float(qx));
                      pz.push_back(float(qz));
                    }
                    double v_target = 0.0;
                    if (drive::auto_brake_target_on_path(px, pz, braking_table, car_spd, v_target)) {
                      in.has_auto_brake_target = true;
                      in.auto_brake_target_speed = v_target;
                    }
                  }
                  guided = true;
                }
                if (!guided) {
                  const traffic::Roads* roads =
                      app.gps_solver.ready() ? &app.gps_solver.roads()
                                             : (!festival_roads.roads().empty() ? &festival_roads : nullptr);
                  if (roads) {
                    const traffic::RoadHit rhit = roads->nearest(car_pos.x, car_pos.z, false, 20.0);
                    if (rhit.road >= 0 && size_t(rhit.road) < roads->roads().size() && rhit.distance < 18.0) {
                      const traffic::Road& rd = roads->roads()[size_t(rhit.road)];
                      std::array<double, 2> dir0{0.0, 1.0};
                      const traffic::P3 p0 = roads->point_at(rhit.road, rhit.s, nullptr, &dir0);
                      const double align = car_fwd.x * dir0[0] + car_fwd.z * dir0[1];
                      if (std::fabs(align) > 0.34 && rd.length > 2.0) {
                        const double travel_sign = align >= 0.0 ? 1.0 : -1.0;
                        const double tx0 = dir0[0] * travel_sign, tz0 = dir0[1] * travel_sign;
                        if (want_lane) {
                          const double lat_off = (car_pos.x - p0.x) * tz0 - (car_pos.z - p0.z) * tx0;
                          std::array<double, 2> dir1 = dir0;
                          roads->point_at(rhit.road, std::clamp(rhit.s + travel_sign * look_d, 0.0, rd.length), nullptr,
                                          &dir1);
                          const double ahead_hdg = std::atan2(dir1[0] * travel_sign, dir1[1] * travel_sign);
                          const double hdg_err = std::remainder(ahead_hdg - car_hdg, 2.0 * kLookPi);
                          if (std::fabs(hdg_err) < 1.2 && std::fabs(lat_off) < 25.0) {
                            in.has_assist_lane = true;
                            in.assist_heading_error_rad = hdg_err;
                            in.assist_lateral_offset_m = lat_off;
                          }
                        }
                        if (want_auto_brake && car_spd > 3.0) {
                          // The road ahead as a path from the car (FEEL-14).
                          std::vector<float> px{float(p0.x)}, pz{float(p0.z)};
                          for (double da = 4.0; da <= kAutoBrakeLookM; da += 4.0) {
                            const double sd = rhit.s + travel_sign * da;
                            if (sd < 0.0 || sd > rd.length) break;
                            const traffic::P3 q = roads->point_at(rhit.road, sd, nullptr, nullptr);
                            px.push_back(float(q.x));
                            pz.push_back(float(q.z));
                          }
                          double v_target = 0.0;
                          if (drive::auto_brake_target_on_path(px, pz, braking_table, car_spd, v_target)) {
                            in.has_auto_brake_target = true;
                            in.auto_brake_target_speed = v_target;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
          rig.set_look(rig_look);
}

}  // namespace carview
