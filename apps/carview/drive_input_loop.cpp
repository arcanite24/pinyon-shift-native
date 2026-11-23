#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "display_hdr_win.h"
#include "settings.h"

namespace carview {

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
                host.showroom.set_world_texture_budget({96u * 1024u * 1024u, 256u});
              }
              continue;
            }
            // Instant alt-tab / focus / occlusion fast path (handled before any overlay
            // continue guards; ZERO wait_idle and ZERO swapchain recreate on focus gain).
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
              host.renderer.set_window_focused(false);
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
                  slice12.sync_pad_capabilities(pads);
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
                  slice12.sync_pad_capabilities(pads);
                  slice12.sync_wheel_devices();
                  break;
                }
              continue;
            }
            if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED) {
              slice12.sync_wheel_devices();
              continue;
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
                  app.show_title || app.title_menu_phase || app.fmv_player.active() || ceremony.active || grid_menu_open ||
                  menu.open || map_open || picking || events_page.is_open() || outpost_hub_open || garage_open ||
                  shop.is_open() || autoshow.is_open() || career_page.is_open() || settings_page.is_open() ||
                  paint_open || club_open || app.rally_hub_open || app.rally_service_open || app.rally_results_open ||
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
                else if (picking) picking = false;
                else if (!app.race && app.pr_stunt_session.active()) {
                  app.pr_stunt_session.cancel();
                  app.gps_nav.clear();
                  notice = "PR STUNT CANCELLED";
                  notice_until = vehicle.time() + 2.5;
                } else open_menu();
              }
              if (!app.race && app.pr_stunt_session.active() && e.key.key == SDLK_P) {
                pr_stunt_photo_pressed = true;
              } else if (e.key.key == SDLK_P && !picking && !(app.race && app.race->player_finished())) {
                const drive::Vec3 pat = vehicle.position();
                const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                                 carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
              }
              if (on_banner && e.key.key == SDLK_BACKSPACE) results_table = false;
              else if (!on_banner && e.key.key == SDLK_BACKSPACE) try_trigger_rewind();
              if (on_banner && e.key.key == SDLK_X) results_table = true;  // X: RACE RESULTS
              if (on_banner && e.key.key == SDLK_V) open_replay_viewer(false);  // V: VIEW REPLAY
              const std::string kn = sdl_key_name(e.key.key);
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
              const drive::ControllerLayout pad_layout = drive::default_controller_layout(live_controls.layout_id);
              const drive::PadControl pc = pad_control_from_button(e.gbutton.button);
              const std::string_view act = pad_layout.action_for(pc);
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) open_menu();
              if (act == "Rewind" && !(app.race && app.race->player_finished()) && !picking) {
                try_trigger_rewind();
              } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                if (rewind_buf.rewind_on_back() && !(app.race && app.race->player_finished()) && !picking) try_trigger_rewind();
                else open_map();
              } else if (act == "Map") {
                open_map();
              }
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH && act != "Rewind" && !rewind_buf.rewind_on_back() &&
                  !(app.race && app.race->player_finished()) && !picking)
                try_trigger_rewind();
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
              if (e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST && !(app.race && app.race->player_finished())) {
                if (picking) take_pick();
                else activate_world_trigger();
              }
              if (app.race && app.race->player_finished() && !picking) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) race_continue();
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) race_restart();
                // X opens the RACE RESULTS table from the banner, B closes it.
                if (results_page == 0 && e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) results_table = true;
                if (results_page == 0 && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) open_replay_viewer(false);
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) results_table = false;
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
  // Left-stick menu navigation with deadzone, axis-dominance lock & repeat
  // for Pause Menu, Settings Menu, and Pause sub-pages/overlays.
  static MenuStickNav menu_stick_nav;
  static bool menu_stick_was_active = false;
  const bool menu_nav_active =
      !pads.empty() &&
      ((settings_page.is_open() && !settings_page.capturing_key()) || menu.open || career_page.is_open() ||
       achievements.screen_open() || mods_page.is_open() || save_page.is_open() ||
       slice12.trainer_overlay.is_open() || slice12.cam_editor_overlay.is_open());
  if (!menu_nav_active) {
    menu_stick_nav.reset();
    menu_stick_was_active = false;
  } else {
    double best_lx = 0.0, best_ly = 0.0, best_mag2 = -1.0;
    for (SDL_Gamepad* pad : pads) {
      const double lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0;
      const double ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0;
      const double mag2 = lx * lx + ly * ly;
      if (mag2 > best_mag2) {
        best_mag2 = mag2;
        best_lx = lx;
        best_ly = ly;
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
      SDL_Event syn{};
      syn.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      syn.gbutton.down = true;
      switch (dir) {
        case MenuStickNav::Dir::kUp: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_UP; break;
        case MenuStickNav::Dir::kDown: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_DOWN; break;
        case MenuStickNav::Dir::kLeft: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_LEFT; break;
        case MenuStickNav::Dir::kRight: syn.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_RIGHT; break;
        case MenuStickNav::Dir::kNone: break;
      }
      if (!handle_dev_overlays_event(syn)) handle_settings_and_media_event(syn);
    }
  }
}

void DriveSession::sample_driving_input(drive::Input& in, double& rig_look) {
          const bool* keys = SDL_GetKeyboardState(nullptr);
          drive::Input kb_in;
          kb_in.steer = (action_key_down(keys, live_controls.keys, drive::ControlAction::kSteerRight) ? 1.0 : 0.0) -
                        (action_key_down(keys, live_controls.keys, drive::ControlAction::kSteerLeft) ? 1.0 : 0.0);
          kb_in.throttle = action_key_down(keys, live_controls.keys, drive::ControlAction::kThrottle) ? 1.0 : 0.0;
          kb_in.brake = action_key_down(keys, live_controls.keys, drive::ControlAction::kBrake) ? 1.0 : 0.0;
          kb_in.handbrake = action_key_down(keys, live_controls.keys, drive::ControlAction::kHandbrake);
          kb_in.clutch = action_key_down(keys, live_controls.keys, drive::ControlAction::kClutch) ? 1.0 : 0.0;
          // Look around: the right stick turns the chase camera up to half a
          // turn, LookBack key held looks back.
          camera_pull.look_target =
              action_key_down(keys, live_controls.keys, drive::ControlAction::kLookBack) ? 3.14159265358979 : 0.0;
          rig_look = camera_pull.look_target;
          const drive::ControllerLayout pad_layout = drive::default_controller_layout(live_controls.layout_id);
          const auto& dz = live_controls.deadzones;
          const drive::PadControl gas_ctrl = pad_layout.control_for("Gas");
          const drive::PadControl brake_ctrl = pad_layout.control_for("Brake");
          const drive::PadControl hb_ctrl = pad_layout.control_for("Handbrake");
          const drive::PadControl clutch_ctrl = pad_layout.control_for("Clutch");
          drive::Input pad_in;
          bool pad_used = false;
          for (SDL_Gamepad* pad : pads) {
            auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0; };
            const char* pname = SDL_GetGamepadName(pad);
            const bool is_wheel_pad = carview::is_racing_wheel_device(
                SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad), pname ? pname : "");
            const double raw_steer = axis(SDL_GAMEPAD_AXIS_LEFTX);
            const double steer =
                is_wheel_pad
                    ? drive::shape_wheel_steering(raw_steer, slice12.wheel_shaping)
                    : drive::apply_deadzone_signed(raw_steer, dz.steering_inside, dz.steering_outside);
            const double raw_throttle = pad_control_value(pad, gas_ctrl, dz.throttle_inside, dz.throttle_outside);
            const double raw_brake = pad_control_value(pad, brake_ctrl, dz.brake_inside, dz.brake_outside);
            const double throttle = is_wheel_pad ? drive::shape_wheel_pedal(raw_throttle, slice12.wheel_shaping) : raw_throttle;
            const double brake = is_wheel_pad ? drive::shape_wheel_pedal(raw_brake, slice12.wheel_shaping) : raw_brake;
            const bool handbrake = pad_control_value(pad, hb_ctrl, dz.handbrake_inside, dz.handbrake_outside) > 0.5;
            const double clutch = pad_control_value(pad, clutch_ctrl, dz.clutch_inside, dz.clutch_outside);
            const double look = drive::apply_deadzone_signed(axis(SDL_GAMEPAD_AXIS_RIGHTX), 0.20, 0.95);
            if (look != 0) camera_pull.look_target = -look * 3.14159265358979;
            // The rig's look (SPEC-CAMERAS, Button): the right stick down
            // looks back, left and right to the sides, its click straight back.
            const double lx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ly = axis(SDL_GAMEPAD_AXIS_RIGHTY);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK) || (ly > 0.5 && ly > std::fabs(lx)))
              rig_look = 3.14159265358979;
            else if (std::fabs(lx) > 0.5 && std::fabs(lx) >= std::fabs(ly))
              rig_look = (lx > 0 ? 0.5 : -0.5) * 3.14159265358979;
            if (steer != 0 || throttle > 0 || brake > 0 || handbrake || clutch > 0) {
              pad_in.steer = steer;
              pad_in.throttle = throttle;
              pad_in.brake = brake;
              pad_in.handbrake = handbrake;
              pad_in.clutch = clutch;
              pad_used = true;
              app.touch_controls.notify_gamepad_input(vehicle.time());
              break;
            }
          }
          const carview::RawJoystickDriveOutput raw_joy = slice12.sample_raw_joystick_input(dz);
          if (!pad_used && raw_joy.any_active) {
            if (raw_joy.look_x != 0.0) camera_pull.look_target = -raw_joy.look_x * 3.14159265358979;
            app.touch_controls.notify_gamepad_input(vehicle.time());
          }
          const carview::RawJoystickDriveOutput wheel_joy = slice12.sample_wheel_input();
          in = carview::merge_driving_inputs(kb_in, pad_in, pad_used, raw_joy.input, raw_joy.any_active,
                                             wheel_joy.input, wheel_joy.any_active);
          slice12.apply_gyro_input(in);
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
            }
          }
          // Auto-Brake (Braking == 0 / Assisted, SPEC-CAR-PHYSICS): query the
          // active racing line's lookahead target speed upstream of stepping.
          if (!app.cruise && live_controls.assists.braking == 0 && app.race && !grid_menu_open && !app.race->player_finished()) {
            if (dl_samples.empty()) {
              drive::build_driving_line(app.race->racing_line(), braking_table, app.race->player_progress(),
                                        std::max(0.0, vehicle.speed()), drive::DrivingLineMode::kBrakingOnly,
                                        dl_samples);
            }
            if (!dl_samples.empty()) {
              drive::apply_auto_brake(in, vehicle.speed(), dl_samples.front().v_target);
            }
          }
          rig.set_look(rig_look);
}

}  // namespace carview
