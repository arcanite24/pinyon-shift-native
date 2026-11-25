#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "settings.h"

namespace carview {

bool DriveSession::ui_mouse_hit(float mx, float my, ui::HitZone& out) {
              int win_w = 0, win_h = 0;
              if (app.window) SDL_GetWindowSize(app.window, &win_w, &win_h);
              const float sx = (win_w > 0 && last_ui_w > 0) ? (float(last_ui_w) / float(win_w)) : 1.0f;
              const float sy = (win_h > 0 && last_ui_h > 0) ? (float(last_ui_h) / float(win_h)) : 1.0f;
              return carview::ui::hit_test_zones(last_ui_hits, mx * sx, my * sy, out);
}

bool DriveSession::handle_campaign_and_title_event(const SDL_Event& e) {
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
              if (e.key.key == SDLK_F3) {
                host.perf_overlay_open = !host.perf_overlay_open;
                host.renderer.set_gpu_timing(host.perf_overlay_open || host.inspector_open);
                return true;
              }
              if (e.key.key == SDLK_F8 && (e.key.mod & SDL_KMOD_CTRL) != 0) {
                host.inspector_open = !host.inspector_open;
                host.renderer.set_inspector_preview(host.inspector_open, host.inspector_view);
                host.renderer.set_gpu_timing(host.perf_overlay_open || host.inspector_open);
                if (!host.inspector_open) host.has_inspector_preview = false;
                return true;
              }
              if (host.inspector_open) {
                int next_v = -1;
                if (e.key.key == SDLK_LEFTBRACKET)
                  next_v = (int(host.inspector_view) + int(carview::InspectorView::kCount) - 1) %
                           int(carview::InspectorView::kCount);
                else if (e.key.key == SDLK_RIGHTBRACKET)
                  next_v = (int(host.inspector_view) + 1) % int(carview::InspectorView::kCount);
                else if (e.key.key >= SDLK_1 && e.key.key <= SDLK_6)
                  next_v = int(e.key.key - SDLK_1);
                if (next_v >= 0) {
                  host.inspector_view = static_cast<carview::InspectorView>(next_v);
                  host.renderer.set_inspector_preview(true, host.inspector_view);
                  return true;
                }
              }
            }
            if (app.skip_vo_with_a && !app.show_title && !menu.open && !settings_page.is_open() && !map_open &&
                !app.fmv_player.active() && app.audio_host.telemetry().vo_playing) {
              const bool pad_a =
                  e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH;
              if (pad_a) {
                app.audio_host.skip_vo();
                if (!app.campaign.is_blocking_gameplay() && app.campaign.flow().dialogs().active())
                  app.campaign.flow().dialogs().skip_current();
              }
            }
            if (app.campaign.is_blocking_gameplay() && !app.show_title) {
              // R3-UI-07: FMVs and cinematics require 0.75 s hold-to-skip in interactive windowed mode
              // (instant skip is only allowed on reward_tally or in --headless / scripted runs).
              const bool require_hold_skip =
                  (app.fmv_player.active() || app.campaign.flow().is_cinematic_active()) &&
                  !app.campaign.flow().reward_tally().active && !app.headless;
              if (require_hold_skip) {
                return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                        e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
              }
              const bool adv_key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                                   (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                                    e.key.key == SDLK_ESCAPE || e.key.key == SDLK_TAB);
              const bool adv_pad = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                   (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                                    e.gbutton.button == SDL_GAMEPAD_BUTTON_START);
              // The tally's help bar shows X for SET GPS: the pad's X does it too.
              const bool gps_key = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_G) ||
                                   (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                    e.gbutton.button == SDL_GAMEPAD_BUTTON_WEST);
              bool mouse_gps = false;
              bool mouse_adv = false;
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui4::kHitButton &&
                      hz.index == 1 && app.campaign.flow().reward_tally().active &&
                      app.campaign.flow().reward_tally().has_next_gps) {
                    mouse_gps = true;
                  } else {
                    mouse_adv = true;
                  }
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                  mouse_adv = true;
                }
              }
              if ((gps_key || mouse_gps) && app.campaign.flow().reward_tally().active &&
                  app.campaign.flow().reward_tally().has_next_gps && app.gps_solver.ready()) {
                const auto& rt = app.campaign.flow().reward_tally();
                const drive::Vec3 at = vehicle.position();
                fire_satnav_event(app.gps_nav.set_destination(rt.next_gps_xz[0], rt.next_gps_xz[1],
                                                          traffic::GpsDestKind::kFestival, rt.next_gps_label, at.x, at.z));
                notice = "GPS SET TO " + rt.next_gps_label;
                notice_until = vehicle.time() + 3.0;
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
              } else if (adv_key || adv_pad || mouse_adv) {
                queued_campaign_actions.push_back(app.campaign.advance_or_skip(app.fmv_player, &app.audio_host));
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (app.campaign.flow().is_onboarding() && e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                e.key.key == SDLK_TAB) {
              queued_campaign_actions.push_back(app.campaign.advance_or_skip(app.fmv_player, &app.audio_host));
              return true;
            }
            if (app.fmv_player.active()) {
              // R3-UI-07: require 0.75 s hold-to-skip in interactive windowed mode (polled in run_window_loop);
              // allow instant skip only in --headless / scripted test mode.
              if (app.headless) {
                const bool skip_key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                                      (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE || e.key.key == SDLK_ESCAPE);
                const bool skip_pad = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                      (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                                       e.gbutton.button == SDL_GAMEPAD_BUTTON_START);
                const bool skip_mouse = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                                        (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT);
                if (skip_key || skip_pad || skip_mouse) {
                  app.fmv_player.skip();
                  app.audio_host.set_fmv_audio(nullptr, nullptr);
                  if (title_attract_playing) {
                    title_attract_playing = false;
                    title_idle_s = 0.0;
                    if (!app.no_title) app.show_title = true;
                  }
                }
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                      e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (app.story_intro.stage() == carview::StoryStage::kBootSplash && e.type == SDL_EVENT_KEY_DOWN &&
                !e.key.repeat && e.key.key == SDLK_TAB) {
              const auto sa = app.story_intro.skip_stage();
              if (sa.play_fmv) {
                const auto mp = carview::FmvPlayer::resolve_movie(app.game_root_of(), sa.fmv_id);
                if (!mp.empty() && app.fmv_player.open(mp, app.language)) {
                  app.audio_host.set_fmv_audio(
                      [](void* ctx, float* out, int fr) { return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, fr); },
                      &app.fmv_player);
                }
              }
              if (sa.enter_title_screen && !app.no_title) app.show_title = true;
              return true;
            }
            // 1. Title Screen input (when settings isn't open on top of it).
            if (app.show_title && !settings_page.is_open()) {
              if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                  e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                title_idle_s = 0.0;
              }
              const bool has_save_opt =
                  app.have_saved && (app.saved.career_started || app.saved.owned_garage || app.career.num_races > 0 || app.career.credits > 0);
              const int item_count = has_save_opt ? 5 : 4;
              auto confirm_title = [&]() {
                if (!app.title_menu_phase) {
                  app.title_menu_phase = true;
                  app.title_selected = 0;
                  return;
                }
                if (app.title_confirm_new) {
                  if (app.title_confirm_selected == 0) start_new_career();
                  else app.title_confirm_new = false;
                  return;
                }
                const int idx = has_save_opt ? app.title_selected : (app.title_selected + 1);
                if (idx == 0) {  // CONTINUE CAREER
                  app.show_title = false;
                  if (app.title_video.active()) app.title_video.close();
                  if (app.title_saved_resume_pending) {
                    app.title_saved_resume_pending = false;
                    loading_screen.load_definitions(app.game_root_of(), "freeroam", app.state_root);
                    loading_screen.start("COLORADO FREE ROAM", "HORIZON FESTIVAL");
                    loading_min_until = 0.25;
                    if (host.streamer) {
                      std::string rerr;
                      bool changed = false;
                      const bool ok = host.streamer->update(app.saved_resume_spawn[0], app.saved_resume_spawn[2],
                                                            &changed, &rerr);
                      if (ok && changed && host.streamer->prepared().has_value()) {
                        host.ground = &host.streamer->ground();
                        host.follower.set_ground(&host.streamer->ground());
                        host.set_staged_world();
                        world::Spawn rsp;
                        const char* rhow = nullptr;
                        if (world::choose_spawn(*host.streamer, app.saved_resume_spawn, nullptr, nullptr, rsp, &rhow)) {
                          vehicle.place({rsp.position[0], rsp.position[1] + 0.03, rsp.position[2]}, rsp.heading);
                          const drive::Vec3 vp = vehicle.position();
                          const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                          drive::Hit wh;
                          if (host.streamer->ground().raycast({vp.x, vp.y + 0.8, vp.z}, {0, 1, 0}, 3.5, wh) ||
                              host.streamer->ground().raycast({vp.x, vp.y + 0.7, vp.z}, vf, 2.5, wh)) {
                            world::recover(*host.streamer, vehicle);
                          }
                        }
                        drawn_before = vehicle.pose();
                      } else if (ok) {
                        // Async gather is running off-thread: keep loading_screen animating at 60 FPS until ready.
                        pending_title_resume_gather = true;
                      }
                    } else {
                      vehicle.place({app.saved_resume_spawn[0], app.saved_resume_spawn[1], app.saved_resume_spawn[2]},
                                    app.saved_resume_spawn[3]);
                      drawn_before = vehicle.pose();
                    }
                  }
                } else if (idx == 1) {  // NEW CAREER
                  if (has_save_opt) {
                    app.title_confirm_new = true;
                    app.title_confirm_selected = 1;
                  } else {
                    if (app.title_video.active()) app.title_video.close();
                    app.title_saved_resume_pending = false;
                    start_new_career();
                  }
                } else if (idx == 2) {  // FREE ROAM
                  app.show_title = false;
                  app.title_saved_resume_pending = false;
                  if (app.title_video.active()) app.title_video.close();
                } else if (idx == 3) {  // SETTINGS
                  settings_page.load(read_settings(settings_path(), nullptr));
                  settings_page.open();
                } else if (idx == 4) {  // QUIT
                  running = false;
                }
              };
              auto back_title = [&]() {
                if (app.title_confirm_new) app.title_confirm_new = false;
                else if (app.title_menu_phase) app.title_menu_phase = false;
                else running = false;
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) confirm_title();
                else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) back_title();
                else if (app.title_confirm_new && (k == SDLK_UP || k == SDLK_W || k == SDLK_DOWN || k == SDLK_S))
                  app.title_confirm_selected = 1 - app.title_confirm_selected;
                else if (app.title_menu_phase && (k == SDLK_UP || k == SDLK_W))
                  app.title_selected = (app.title_selected + item_count - 1) % item_count;
                else if (app.title_menu_phase && (k == SDLK_DOWN || k == SDLK_S))
                  app.title_selected = (app.title_selected + 1) % item_count;
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_START) confirm_title();
                else if (b == SDL_GAMEPAD_BUTTON_EAST) back_title();
                else if (app.title_confirm_new &&
                         (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
                          b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
                  app.title_confirm_selected = 1 - app.title_confirm_selected;
                else if (app.title_menu_phase && b == SDL_GAMEPAD_BUTTON_DPAD_UP)
                  app.title_selected = (app.title_selected + item_count - 1) % item_count;
                else if (app.title_menu_phase && b == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                  app.title_selected = (app.title_selected + 1) % item_count;
              }
              // Raw SDL_Joystick fallback navigation on title screen (for unmapped USB/BT pads).
              if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && !SDL_IsGamepad(e.jbutton.which)) {
                const Uint8 b = e.jbutton.button;
                if (b == 0 || b == 7 || b == 9 || b == 11) confirm_title();
                else if (b == 1) back_title();
              }
              if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && !SDL_IsGamepad(e.jhat.which)) {
                if (app.title_confirm_new && (e.jhat.value == SDL_HAT_UP || e.jhat.value == SDL_HAT_DOWN))
                  app.title_confirm_selected = 1 - app.title_confirm_selected;
                else if (app.title_menu_phase && e.jhat.value == SDL_HAT_UP)
                  app.title_selected = (app.title_selected + item_count - 1) % item_count;
                else if (app.title_menu_phase && e.jhat.value == SDL_HAT_DOWN)
                  app.title_selected = (app.title_selected + 1) % item_count;
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz)) {
                  if (app.title_confirm_new && (hz.group == carview::ui::kHitConfirm || hz.group == carview::ui::kHitCancel)) {
                    const int next_sel = (hz.group == carview::ui::kHitConfirm) ? 0 : 1;
                    if (app.title_confirm_selected != next_sel) {
                      app.title_confirm_selected = next_sel;
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    }
                  } else if (app.title_menu_phase && hz.group == carview::ui::kHitRow && hz.index >= 0 && hz.index < item_count) {
                    if (app.title_selected != hz.index) {
                      app.title_selected = hz.index;
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  back_title();
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  if (!app.title_menu_phase) {
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    confirm_title();
                  } else {
                    carview::ui::HitZone hz{};
                    if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                      if (app.title_confirm_new && (hz.group == carview::ui::kHitConfirm || hz.group == carview::ui::kHitCancel)) {
                        app.title_confirm_selected = (hz.group == carview::ui::kHitConfirm) ? 0 : 1;
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        confirm_title();
                      } else if (!app.title_confirm_new && hz.group == carview::ui::kHitRow && hz.index >= 0 && hz.index < item_count) {
                        app.title_selected = hz.index;
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        confirm_title();
                      }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL && app.title_menu_phase && !app.title_confirm_new) {
                if (e.wheel.y > 0) app.title_selected = (app.title_selected + item_count - 1) % item_count;
                else if (e.wheel.y < 0) app.title_selected = (app.title_selected + 1) % item_count;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return true;
            }
            // 2. Wristband Collection Ceremony overlay.
            if (ceremony.active) {
              if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                   (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                    e.key.key == SDLK_ESCAPE)) ||
                  (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                   (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST))) {
                ceremony.active = false;
                // Refresh Events page if it was open underneath.
                if (events_page.is_open()) open_events_browser(events_page.hub_filter());
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // 3. Pre-Race Grid Menu (race-grid.png).
  return false;
}

bool DriveSession::handle_grid_and_map_event(const SDL_Event& e) {
            if (app.race && grid_menu_open && !menu.open && !settings_page.is_open()) {
              auto confirm_grid = [&]() {
                if (grid_menu_selected == 0) {  // START RACE
                  grid_menu_open = false;
                  pre_race_blend_t = 0.0;
                } else if (grid_menu_selected == 1) {  // DIFFICULTY
                  cycle_difficulty(+1);
                } else {  // QUIT RACE
                  grid_menu_open = false;
                  app.restore_pre_race_tod();
                  app.leave_race = true;
                  running = false;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_UP || k == SDLK_W) grid_menu_selected = (grid_menu_selected + 2) % 3;
                else if (k == SDLK_DOWN || k == SDLK_S) grid_menu_selected = (grid_menu_selected + 1) % 3;
                else if (grid_menu_selected == 1 && (k == SDLK_LEFT || k == SDLK_A)) cycle_difficulty(-1);
                else if (grid_menu_selected == 1 && (k == SDLK_RIGHT || k == SDLK_D)) cycle_difficulty(+1);
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) confirm_grid();
                else if (k == SDLK_ESCAPE) {
                  grid_menu_open = false;
                  app.restore_pre_race_tod();
                  app.leave_race = true;
                  running = false;
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) grid_menu_selected = (grid_menu_selected + 2) % 3;
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) grid_menu_selected = (grid_menu_selected + 1) % 3;
                else if (grid_menu_selected == 1 && b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) cycle_difficulty(-1);
                else if (grid_menu_selected == 1 && b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) cycle_difficulty(+1);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) confirm_grid();
                else if (b == SDL_GAMEPAD_BUTTON_EAST) {
                  grid_menu_open = false;
                  app.restore_pre_race_tod();
                  app.leave_race = true;
                  running = false;
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) &&
                    (hz.group == carview::ui::kHitRow || hz.group == carview::ui::kHitStepLeft ||
                     hz.group == carview::ui::kHitStepRight) &&
                    hz.index >= 0 && hz.index < 3 && grid_menu_selected != hz.index) {
                  grid_menu_selected = hz.index;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                  if (hz.group == carview::ui::kHitStepLeft && hz.index == 1) {
                    grid_menu_selected = 1;
                    cycle_difficulty(-1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (hz.group == carview::ui::kHitStepRight && hz.index == 1) {
                    grid_menu_selected = 1;
                    cycle_difficulty(+1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (hz.group == carview::ui::kHitRow && hz.index >= 0 && hz.index < 3) {
                    grid_menu_selected = hz.index;
                    if (hz.index == 1) {
                      float ux = e.button.x, uy = e.button.y;
                      if (app.window) {
                        int ww = 0, wh = 0;
                        SDL_GetWindowSize(app.window, &ww, &wh);
                        if (ww > 0 && wh > 0 && last_ui_w > 0 && last_ui_h > 0) {
                          ux = e.button.x * float(last_ui_w) / float(ww);
                          uy = e.button.y * float(last_ui_h) / float(wh);
                        }
                      }
                      (void)uy;
                      const float span = std::max(1.0f, hz.x1 - hz.x0);
                      const float frac = (ux - hz.x0) / span;
                      cycle_difficulty(frac < 0.35f ? -1 : +1);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else {
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      confirm_grid();
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) grid_menu_selected = (grid_menu_selected + 2) % 3;
                else if (e.wheel.y < 0) grid_menu_selected = (grid_menu_selected + 1) % 3;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // 4. Interactive Pause Map (1:1 retail FH1 controls: A/Enter/Click sets GPS
            //    keeping the map open, X/F Fast Travels or clears GPS, Y/Tab centers on car,
            //    LB/RB or Q/E cycles filters, LT/RT/wheel zooms, B/Esc/M/Back closes).
            if (map_open) {
              bool close = false;
              auto zoom_by = [&](double f) {
                map_cam.nudge_target_zoom(f);
                map_dirty = true;
              };
              auto pan_by = [&](double dx, double dz) {
                map_cam.pan_direct(dx, dz);
                map_cx = map_cam.cx();
                map_cz = map_cam.cz();
                map_dirty = true;
                update_map_selection();
              };
              auto map_center_on_car = [&]() {
                const drive::Vec3 at = vehicle.position();
                map_cam.reset(at.x, at.z, map_zoom);
                map_cx = map_cam.cx();
                map_cz = map_cam.cz();
                map_zoom = map_cam.zoom();
                map_dirty = true;
                update_map_selection();
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto resolve_gps_target = [&](double& tx, double& tz, std::string& label, traffic::GpsDestKind& dk) {
                tx = map_cx;
                tz = map_cz;
                label = "CUSTOM WAYPOINT";
                dk = traffic::GpsDestKind::kGeneric;
                if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
                  const auto& m = cached_map_markers[size_t(map_selected_marker)];
                  tx = m.x;
                  tz = m.z;
                  label = m.title;
                  if (m.kind == MapMarker::kEvent) {
                    dk = m.icon_tag == "exhibition" ? traffic::GpsDestKind::kShowcase
                                                    : traffic::GpsDestKind::kFestivalRace;
                  } else if (m.kind == MapMarker::kStreetHub) {
                    dk = traffic::GpsDestKind::kStreetRace;
                  } else if (m.kind == MapMarker::kOutpost) {
                    dk = traffic::GpsDestKind::kOutpost;
                  } else if (m.kind == MapMarker::kFestivalSite) {
                    const auto sk = race::FestivalSite::Kind(m.site_or_hub);
                    if (sk == race::FestivalSite::kRaceCentral) dk = traffic::GpsDestKind::kRaceCentral;
                    else if (sk == race::FestivalSite::kAutoshow) dk = traffic::GpsDestKind::kAutoshow;
                    else if (sk == race::FestivalSite::kWorkshop) dk = traffic::GpsDestKind::kWorkshop;
                    else if (sk == race::FestivalSite::kPaintShop) dk = traffic::GpsDestKind::kPaintShop;
                  }
                  return;
                }
                const bool show_barns = map_filter == MapFilter::kAll || map_filter == MapFilter::kDiscovery;
                const bool show_signs = map_filter == MapFilter::kAll || map_filter == MapFilter::kSigns ||
                                        map_filter == MapFilter::kDiscovery;
                if ((show_barns || show_signs) && app.progress_loaded) {
                  const bool reveal = app.reveal || app.treasure_map_setting ||
                                      slice12.trainer.effective_reveal_collectibles();
                  double best_cd = 280.0 / std::max(0.5, map_zoom);
                  if (show_barns) {
                    for (size_t bi = 0; bi < app.collectibles.barns.size(); ++bi) {
                      const auto& b = app.collectibles.barns[bi];
                      const int st = bi < app.progress.barns.size() ? app.progress.barns[bi].status : freeroam::kBarnNotSpawned;
                      if (st != freeroam::kBarnFound && st != freeroam::kBarnRumour && !reveal) continue;
                      const double d = std::hypot(b.at.x - map_cx, b.at.z - map_cz);
                      if (d < best_cd) {
                        best_cd = d;
                        tx = b.at.x;
                        tz = b.at.z;
                        label = b.name.empty() ? "BARN FIND" : b.name;
                      }
                    }
                  }
                  if (show_signs) {
                    for (const auto& sign : app.collectibles.signs) {
                      const bool found = app.progress.signs_found.count(sign.id) > 0;
                      if (!found && !reveal) continue;
                      const double d = std::hypot(sign.at.x - map_cx, sign.at.z - map_cz);
                      if (d < best_cd) {
                        best_cd = d;
                        tx = sign.at.x;
                        tz = sign.at.z;
                        label = "DISCOUNT SIGN";
                      }
                    }
                  }
                }
              };
              auto map_set_gps = [&]() {
                if (app.race != nullptr) {
                  notice = "GPS UNAVAILABLE DURING RACE";
                  notice_until = vehicle.time() + 2.5;
                  app.audio_host.trigger_ui(audio::UiCue::kError);
                  return;
                }
                if (!app.gps_solver.ready()) return;
                const drive::Vec3 at = vehicle.position();
                double tx = map_cx, tz = map_cz;
                std::string label;
                traffic::GpsDestKind dk = traffic::GpsDestKind::kGeneric;
                resolve_gps_target(tx, tz, label, dk);
                // Toggle off if pressing A/Enter/Click again on the active GPS destination;
                // ALWAYS keep the World Map open so the player can inspect the GPS route!
                if (app.gps_nav.active() &&
                    std::hypot(tx - app.gps_nav.route().dest_x, tz - app.gps_nav.route().dest_z) < 120.0) {
                  app.gps_nav.clear();
                  app.audio_host.trigger_satnav(audio::SatNavCue::kDestReset);
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else {
                  const auto gev = app.gps_nav.set_destination(tx, tz, dk, label, at.x, at.z);
                  fire_satnav_event(gev);
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                }
                map_dirty = true;
                update_map_selection();
              };
              auto map_fast_travel_or_secondary = [&]() {
                const drive::Vec3 at = vehicle.position();
                const bool ft_anywhere = slice12.trainer.effective_fast_travel_anywhere();
                double target_x = map_cx, target_z = map_cz;
                std::string target_lbl;
                traffic::GpsDestKind target_dk = traffic::GpsDestKind::kGeneric;
                resolve_gps_target(target_x, target_z, target_lbl, target_dk);
                const bool hovering_gps_dest =
                    app.gps_nav.active() &&
                    std::hypot(target_x - app.gps_nav.route().dest_x, target_z - app.gps_nav.route().dest_z) < 120.0;

                if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
                  const MapMarker m = cached_map_markers[size_t(map_selected_marker)];
                  const double player_dist = std::hypot(m.x - at.x, m.z - at.z);
                  if (m.kind == MapMarker::kOutpost && app.have_outposts) {
                    if (const auto* op = app.outposts.find_outpost(m.id)) {
                      if (player_dist <= std::max(180.0, op->discovery_radius)) {
                        close_map();
                        open_outpost_hub(op->id);
                        return;
                      }
                      if (app.outpost_progress.is_outpost_discovered(op->id) || ft_anywhere) {
                        if (app.race != nullptr) {
                          notice = "QUIT RACE TO FAST TRAVEL";
                          notice_until = vehicle.time() + 3.0;
                          app.audio_host.trigger_ui(audio::UiCue::kError);
                          return;
                        }
                        // The outpost's fast-travel price (GlobalRegistry Activities/Outposts:
                        // 10,000 / 7,500 / 5,000 CR, free with its 3 PR Stunts done).
                        const int64_t ft_fee =
                            ft_anywhere ? 0
                                        : std::max<int64_t>(0, app.outpost_progress.fast_travel_cost(
                                                                   app.outposts, op->id, app.fast_travel_tuning));
                        if (ft_fee > 0 && app.career.credits < ft_fee) {
                          notice = "NEED " + carview::thousands(ft_fee) + " CR TO FAST TRAVEL";
                          notice_until = vehicle.time() + 3.5;
                          app.audio_host.trigger_ui(audio::UiCue::kError);
                          return;
                        }
                        if (ft_fee > 0) {
                          app.career.credits -= ft_fee;
                          save_now("fast travel");
                        }
                        close_map();
                        app.spawn[0] = op->spawn.x;
                        app.spawn[1] = 0.0;
                        app.spawn[2] = op->spawn.z;
                        app.spawn[3] = std::atan2(op->spawn.z_axis[0], op->spawn.z_axis[2]);
                        app.have_spawn = true;
                        app.fast_travel = true;
                        app.next_car = app.car;
                        running = false;
                        return;
                      }
                      if (app.gps_nav.active() && !hovering_gps_dest) {
                        app.gps_nav.clear();
                        app.audio_host.trigger_satnav(audio::SatNavCue::kDestReset);
                        map_dirty = true;
                        update_map_selection();
                        return;
                      }
                      notice = "DRIVE TO OUTPOST TO UNLOCK FAST TRAVEL";
                      notice_until = vehicle.time() + 3.5;
                      app.audio_host.trigger_ui(audio::UiCue::kError);
                      return;
                    }
                  } else if (m.kind == MapMarker::kFestivalSite) {
                    if (player_dist <= 180.0) {
                      close_map();
                      const auto kind = race::FestivalSite::Kind(m.site_or_hub);
                      if (kind == race::FestivalSite::kRaceCentral) {
                        if (!collect_next_wristband()) open_events_browser(0);
                      } else if (kind == race::FestivalSite::kAutoshow) open_shop_or_autoshow(true);
                      else if (kind == race::FestivalSite::kWorkshop) open_shop_or_autoshow(false);
                      else if (kind == race::FestivalSite::kPaintShop) open_paint();
                      return;
                    }
                    if (app.race != nullptr) {
                      notice = "QUIT RACE TO FAST TRAVEL";
                      notice_until = vehicle.time() + 3.0;
                      app.audio_host.trigger_ui(audio::UiCue::kError);
                      return;
                    }
                    close_map();
                    app.spawn[0] = m.x;
                    app.spawn[1] = 0.0;
                    app.spawn[2] = m.z;
                    app.spawn[3] = 0.0;
                    app.have_spawn = true;
                    app.fast_travel = true;
                    app.next_car = app.car;
                    running = false;
                    return;
                  } else if (m.kind == MapMarker::kEvent && player_dist <= 180.0) {
                    close_map();
                    if (const auto* ev = app.career_catalog.find_event(m.id)) try_enter_event(*ev);
                    return;
                  } else if (m.kind == MapMarker::kStreetHub && player_dist <= 180.0) {
                    close_map();
                    open_events_browser(m.site_or_hub);
                    return;
                  } else if (m.kind == MapMarker::kPrStunt && player_dist <= 180.0 && app.have_outposts) {
                    if (const auto* st = app.outposts.find_stunt(m.id)) {
                      if (app.race == nullptr) {
                        close_map();
                        start_pr_stunt(*st);
                      }
                    }
                    return;
                  }
                }

                if (ft_anywhere) {
                  if (app.race != nullptr) {
                    notice = "QUIT RACE TO FAST TRAVEL";
                    notice_until = vehicle.time() + 3.0;
                    app.audio_host.trigger_ui(audio::UiCue::kError);
                    return;
                  }
                  close_map();
                  app.spawn[0] = target_x;
                  app.spawn[1] = 0.0;
                  app.spawn[2] = target_z;
                  app.spawn[3] = 0.0;
                  app.have_spawn = true;
                  app.fast_travel = true;
                  app.next_car = app.car;
                  running = false;
                  return;
                }

                if (app.gps_nav.active() && !hovering_gps_dest) {
                  app.gps_nav.clear();
                  app.audio_host.trigger_satnav(audio::SatNavCue::kDestReset);
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  map_dirty = true;
                  update_map_selection();
                  return;
                }

                map_center_on_car();
              };

              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (!e.key.repeat && (k == SDLK_M || k == SDLK_ESCAPE)) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  close = true;
                }
                if (!e.key.repeat && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_G)) {
                  map_set_gps();
                }
                if (!e.key.repeat && k == SDLK_F) map_fast_travel_or_secondary();
                if (!e.key.repeat && k == SDLK_TAB) map_center_on_car();
                if (!e.key.repeat && k == SDLK_C && app.gps_nav.active()) {
                  app.gps_nav.clear();
                  app.audio_host.trigger_satnav(audio::SatNavCue::kDestReset);
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  map_dirty = true;
                  update_map_selection();
                }
                if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) zoom_by(1.35);
                if (k == SDLK_MINUS || k == SDLK_KP_MINUS) zoom_by(1 / 1.35);
                if (!e.key.repeat && k == SDLK_Q) {
                  map_filter = MapFilter((int(map_filter) + int(MapFilter::kCount) - 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (!e.key.repeat && k == SDLK_E) {
                  map_filter = MapFilter((int(map_filter) + 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                // WASD / arrow panning is polled continuously each frame via MapCameraController (no discrete jumps!).
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) zoom_by(1.22);
                else if (e.wheel.y < 0) zoom_by(1.0 / 1.22);
              }
              auto screen_click_to_map = [&](float mx, float my) {
                int win_w = 0, win_h = 0;
                if (app.window) SDL_GetWindowSize(app.window, &win_w, &win_h);
                if (win_w > 64 && win_h > 64 && map_w > 64 && map_h > 64) {
                  const double sx = double(mx) * (double(map_w) / double(win_w));
                  const double sy = double(my) * (double(map_h) / double(win_h));
                  const double mpp = app.map_view.map_scale(map_w, map_h, map_zoom);
                  const double dx = (sx - 0.5 * double(map_w)) * mpp;
                  const double dz = -(sy - 0.5 * double(map_h)) * mpp;
                  pan_by(dx, dz);
                  map_set_gps();
                }
              };
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                // R3-UI-12: guard map click / drag against UI filter tabs, legend toggle, and bottom bar hits.
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                  map_dragging = false;
                  map_drag_moved = false;
                  if (hz.group == carview::ui::kHitTab && hz.index >= 0 && hz.index < int(MapFilter::kCount)) {
                    map_filter = MapFilter(hz.index);
                    map_dirty = true;
                    update_map_selection();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (hz.group == carview::ui4::kHitButton) {
                    if (hz.index == 100) {
                      map_overlay.show_legend = !map_overlay.show_legend;
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.index == 0) {
                      map_set_gps();
                    } else if (hz.index == 1) {
                      map_fast_travel_or_secondary();
                    } else if (hz.index == 2) {
                      map_filter = MapFilter((int(map_filter) + 1) % int(MapFilter::kCount));
                      map_dirty = true;
                      update_map_selection();
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.index == 3) {
                      zoom_by(1.25);
                    } else if (hz.index == 4) {
                      app.audio_host.trigger_ui(audio::UiCue::kCancel);
                      close = true;
                    }
                  }
                } else {
                  map_dragging = true;
                  map_drag_moved = false;
                  map_drag_start_x = e.button.x;
                  map_drag_start_y = e.button.y;
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT) {
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
                close = true;
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                const bool was_click = map_dragging && !map_drag_moved;
                map_dragging = false;
                map_drag_moved = false;
                carview::ui::HitZone up_hz{};
                if (was_click && !ui_mouse_hit(e.button.x, e.button.y, up_hz)) {
                  screen_click_to_map(e.button.x, e.button.y);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION && map_dragging && map_w > 64 && map_h > 64) {
                if (!map_drag_moved &&
                    std::hypot(double(e.motion.x - map_drag_start_x), double(e.motion.y - map_drag_start_y)) > 6.0) {
                  map_drag_moved = true;
                }
                int win_w = map_w, win_h = map_h;
                if (app.window) SDL_GetWindowSize(app.window, &win_w, &win_h);
                const double dpi_x = (win_w > 64) ? (double(map_w) / double(win_w)) : 1.0;
                const double dpi_y = (win_h > 64) ? (double(map_h) / double(win_h)) : 1.0;
                const double mpp = app.map_view.map_scale(map_w, map_h, map_zoom);
                pan_by(-double(e.motion.xrel) * dpi_x * mpp, double(e.motion.yrel) * dpi_y * mpp);
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_BACK) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  close = true;
                }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) map_set_gps();
                if (b == SDL_GAMEPAD_BUTTON_WEST) map_fast_travel_or_secondary();
                if (b == SDL_GAMEPAD_BUTTON_NORTH) map_center_on_car();
                if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  map_filter = MapFilter((int(map_filter) + int(MapFilter::kCount) - 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  map_filter = MapFilter((int(map_filter) + 1) % int(MapFilter::kCount));
                  map_dirty = true;
                  update_map_selection();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                // D-pad panning is polled continuously each frame via MapCameraController (no discrete jumps!).
              }
              if (close) {
                map_dragging = false;
                close_map();
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // 5. Race Central / Events / Street Hub browser.
  return false;
}

bool DriveSession::handle_events_and_outpost_event(const SDL_Event& e) {
            if (events_page.is_open()) {
              auto confirm_event = [&]() {
                const auto* sel = events_page.selected_entry();
                if (!sel) return;
                if (sel->is_collect_wristband) {
                  collect_next_wristband();
                  return;
                }
                if (!sel->unlocked) {
                  notice = sel->reason;
                  notice_until = vehicle.time() + 3.5;
                  return;
                }
                if (sel->event) try_enter_event(*sel->event);
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) {
                  events_page.close();
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_UP || k == SDLK_W) {
                  events_page.move(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_DOWN || k == SDLK_S) {
                  events_page.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_Q) {
                  events_page.switch_tab(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_E) {
                  events_page.switch_tab(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm_event();
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  events_page.close();
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  events_page.move(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  events_page.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  events_page.switch_tab(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  events_page.switch_tab(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm_event();
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0 &&
                    size_t(hz.index) != events_page.selected_index()) {
                  events_page.select_index(size_t(hz.index));
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  events_page.close();
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitTab) {
                      events_page.set_tab(hz.index);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                      events_page.select_index(size_t(hz.index));
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      confirm_event();
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) events_page.move(-1);
                else if (e.wheel.y < 0) events_page.move(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // 5b. Regional Horizon Outpost Hub menu.
            if (outpost_hub_open) {
              const size_t n = outpost_hub_items.size();
              auto confirm_outpost_item = [&]() {
                if (!n || outpost_hub_pick >= n) return;
                const auto item = outpost_hub_items[outpost_hub_pick];
                outpost_hub_open = false;
                switch (item.kind) {
                  case OutpostHubItem::kPrStunt:
                    if (const auto* st = app.outposts.find_stunt(item.ref_id)) start_pr_stunt(*st);
                    break;
                  case OutpostHubItem::kGarage:
                    open_garage_browser();
                    break;
                  case OutpostHubItem::kNearbyEvents:
                    open_events_browser(0);
                    break;
                  case OutpostHubItem::kFastTravelFestival:
                  case OutpostHubItem::kFastTravelOutpost:
                    if (app.race != nullptr) {
                      notice = "QUIT RACE TO FAST TRAVEL";
                      notice_until = vehicle.time() + 3.0;
                    } else {
                      // The festival is always free; an outpost costs its fast-travel price.
                      const bool ft_free = (item.kind == OutpostHubItem::kFastTravelFestival) ||
                                           slice12.trainer.effective_fast_travel_anywhere() || !app.have_outposts;
                      const int64_t ft_fee =
                          ft_free ? 0
                                  : std::max<int64_t>(0, app.outpost_progress.fast_travel_cost(
                                                             app.outposts, item.ref_id, app.fast_travel_tuning));
                      if (ft_fee > 0 && app.career.credits < ft_fee) {
                        notice = "NEED " + carview::thousands(ft_fee) + " CR TO FAST TRAVEL";
                        notice_until = vehicle.time() + 3.5;
                        app.audio_host.trigger_ui(audio::UiCue::kError);
                      } else {
                        if (ft_fee > 0) {
                          app.career.credits -= ft_fee;
                          save_now("fast travel");
                        }
                        app.spawn[0] = item.dest_x;
                        app.spawn[1] = 0.0;
                        app.spawn[2] = item.dest_z;
                        app.spawn[3] = 0.0;
                        app.have_spawn = true;
                        app.fast_travel = true;
                        app.next_car = app.car;
                        running = false;
                      }
                    }
                    break;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) {
                  outpost_hub_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (n && (k == SDLK_UP || k == SDLK_W || k == SDLK_LEFT || k == SDLK_A)) {
                  outpost_hub_pick = (outpost_hub_pick + n - 1) % n;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (n && (k == SDLK_DOWN || k == SDLK_S || k == SDLK_RIGHT || k == SDLK_D)) {
                  outpost_hub_pick = (outpost_hub_pick + 1) % n;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm_outpost_item();
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  outpost_hub_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (n && (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_LEFT)) {
                  outpost_hub_pick = (outpost_hub_pick + n - 1) % n;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (n && (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) {
                  outpost_hub_pick = (outpost_hub_pick + 1) % n;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm_outpost_item();
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION && n > 0) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow &&
                    hz.index >= 0 && size_t(hz.index) < n && outpost_hub_pick != size_t(hz.index)) {
                  outpost_hub_pick = size_t(hz.index);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  outpost_hub_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT && n > 0) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui::kHitRow &&
                      hz.index >= 0 && size_t(hz.index) < n) {
                    outpost_hub_pick = size_t(hz.index);
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    confirm_outpost_item();
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL && n > 0) {
                if (e.wheel.y > 0) outpost_hub_pick = (outpost_hub_pick + n - 1) % n;
                else if (e.wheel.y < 0) outpost_hub_pick = (outpost_hub_pick + 1) % n;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // 6. Garage car browser (horizontal carousel + Sort/Filter/Sell Car).
  return false;
}

}  // namespace carview
