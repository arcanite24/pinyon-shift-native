#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "display_hdr_win.h"
#include "fh1data/cjk_font.h"
#include "overlay_input.h"
#include "settings.h"

namespace carview {

bool DriveSession::handle_settings_and_media_event(const SDL_Event& e) {
            if (settings_page.is_open()) {
              auto apply_live = [&]() {
                const LiveSettings ls = settings_page.live_settings();
                live_controls = ls.controls;
                rumble_enabled = ls.controls.rumble_enabled;
                rumble_scale = ls.controls.rumble_scale;
                if (!rumble_enabled || rumble_scale <= 0.0) {
                  for (SDL_Gamepad* pad : pads) SDL_RumbleGamepad(pad, 0, 0, 0);
                }
                if (ls.camera_mode >= 0 && have_rig) {
                  rig.set_mode(drive::CameraMode(std::clamp(ls.camera_mode, 0, drive::kCameraModes - 1)));
                }
                if (have_rig && ls.controls.camera_effects != loaded_camera_effects) {
                  const drive::CameraMode cur_mode = rig.mode();
                  std::string cam_note;
                  if (load_camera_rig(app.game_root_of(), app.bake_dir, info, vehicle, rig, cam_note,
                                      ls.controls.camera_effects)) {
                    rig.set_mode(cur_mode);
                    loaded_camera_effects = ls.controls.camera_effects;
                  }
                }
                if (ls.difficulty >= 0 && ls.difficulty <= 4) app.race_options.difficulty = race::Difficulty(ls.difficulty);
                app.race_options.ai_mode = race::AiMode(std::clamp(ls.ai_mode, 0, 2));
                app.race_options.rubberband = ls.rubberband;
                if (app.race) {
                  app.race->set_difficulty(app.race_options.difficulty);
                  app.race->set_ai_mode(app.race_options.ai_mode);
                }
                live_sound_enabled = ls.sound;
                live_radio_enabled = ls.radio;
                app.treasure_map_setting = ls.treasure_map;
                live_motion_blur = ls.motion_blur;
                live_dof = ls.dof;
                app.skip_intro_movies = ls.skip_intro_movies;
                app.start_in_single_player = ls.start_in_single_player;
                app.skip_vo_with_a = ls.skip_vo_with_a;
                app.metric_units = ls.metric_units;
                if (!ls.language.empty() && ls.language != app.language) {
                  app.language = ls.language;
                  app.race_options.language = app.language;
                  std::string le;
                  if (app.car_names) app.car_names->load(app.game_root_of(), app.state_root, &le, app.language, app.shared_db);
                  if (fh1::CjkFont::language_needs_cjk(app.language)) {
                    auto cf = std::make_shared<fh1::CjkFont>();
                    if (cf->load_for_language(app.game_root_of(), app.language, nullptr)) carview::ui::set_cjk_font(cf);
                  } else {
                    carview::ui::set_cjk_font(nullptr);
                  }
                  ui_atlas_synced = false;
                }
                live_photo_through_walls = ls.photo_through_walls;
                live_photo_wider_limits = ls.photo_wider_limits;
                photo_mode.set_limits(
                    carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
                app.audio_host.apply_config(carview::AudioConfig::from_tokens(settings_page.tokens()), app.user_music_dir);
                apply_vehicle_assists(vehicle, live_controls.assists);
                rewind_buf.set_enabled(live_controls.assists.rewind);
                driving_line_mode =
                    static_cast<drive::DrivingLineMode>(std::clamp(live_controls.assists.driving_line, 0, 2));
                app.race_options.assists = to_difficulty_assists(live_controls.assists);
                if (app.race) app.race->set_assists(app.race_options.assists);
                app.tod_clock.speed_scale = ls.day_length_scale;
                if (ls.tod_preset_hour >= 0.0) {
                  app.tod_clock.set_hour(ls.tod_preset_hour);
                  if (traffic) traffic->set_hour(app.tod_clock.hour());
                  host.renderer.reset_auto_exposure();
                }
                slice12.apply_live_settings(ls, rig, settings_path().parent_path());
                slice12.sync_pad_capabilities(pads);
                if (ls.graphics != host.active_graphics) {
                  const bool lut_changed = ls.graphics.lut != host.active_graphics.lut;
                  const bool ahdr_changed = ls.graphics.auto_hdr != host.active_graphics.auto_hdr;
                  host.active_graphics = ls.graphics;
                  host.renderer.set_graphics_settings(host.active_graphics);
                  host.showroom.set_texture_cap(host.active_graphics.texture_cap);
                  host.frame_pacer.set_interval(carview::modern::fps_cap_interval(host.active_graphics.fps_cap, host.display_hz));
                  if (lut_changed) {
                    if (host.active_graphics.lut.empty()) {
                      host.user_lut_active = false;
                      last_lut_zone = -2;
                    } else {
                      std::string lut_err;
                      if (apply_user_cube_lut(host.active_graphics.lut, carview::settings_path().parent_path(), host.renderer,
                                              lut_err)) {
                        host.user_lut_active = true;
                      } else {
                        host.user_lut_active = false;
                        last_lut_zone = -2;
                        notice = lut_err;
                        notice_until = vehicle.time() + 3.0;
                      }
                    }
                  }
                  if (ahdr_changed && app.window) {
                    if (host.active_graphics.auto_hdr) {
                      const auto st_dir = carview::settings_path().parent_path();
                      if (!st_dir.empty()) carview::set_display_hdr_restore_file(st_dir / "hdr_restore.bin");
                      carview::turn_on_display_hdr();
                    } else {
                      carview::restore_display_hdr();
                    }
                  }
                }
              };
              auto confirm = [&]() {
                if (settings_page.on_rebind_row()) {
                  settings_page.begin_rebind();
                  return;
                }
                if (!settings_page.on_save_line()) return;
                apply_live();
                std::string why;
                const auto file = settings_path();
                if (file.empty()) why = "no user folder";
                else if (!write_settings(file, settings_page.tokens(), &why)) why = why.empty() ? "not written" : why;
                save_now("settings");
                notice = why.empty() ? "settings saved and applied" : "settings: " + why;
                notice_until = vehicle.time() + 4;
                if (why.empty()) settings_page.close();
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (settings_page.capturing_key()) {
                  if (!e.key.repeat) {
                    if (k == SDLK_ESCAPE) {
                      settings_page.cancel_rebind();
                    } else {
                      const std::string kn = sdl_key_name(k);
                      if (!kn.empty() && settings_page.commit_rebind(kn)) apply_live();
                    }
                  }
                } else {
                  if ((k == SDLK_ESCAPE || k == SDLK_F6) && !e.key.repeat) {
                    settings_page.close();
                    settings_live_overlay = false;
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    confirm();
                  } else if (k == SDLK_UP || k == SDLK_W) {
                    settings_page.move(-1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_DOWN || k == SDLK_S) {
                    settings_page.move(1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_Q) {
                    settings_page.switch_tab(-1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_M && !e.key.repeat) {
                    mods_page.open(app.mods_session);  // F6 > MODS
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  } else if (k == SDLK_P && !e.key.repeat) {
                    save_page.open(app.save_session, app.save_session.sync_report().conflict
                                                     ? carview::SaveProfilePage::Tab::kSync
                                                     : carview::SaveProfilePage::Tab::kBackups);
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  } else if (k == SDLK_E || k == SDLK_TAB) {
                    settings_page.switch_tab((e.key.mod & SDL_KMOD_SHIFT) ? -1 : +1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_LEFT || k == SDLK_A) {
                    settings_page.step(-1);
                    apply_live();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_RIGHT || k == SDLK_D) {
                    settings_page.step(1);
                    apply_live();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !settings_page.capturing_key()) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  settings_page.close();
                  settings_live_overlay = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  confirm();
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  settings_page.move(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  settings_page.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  settings_page.switch_tab(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  mods_page.open(app.mods_session);  // F6 > MODS
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                  save_page.open(app.save_session, app.save_session.sync_report().conflict
                                                   ? carview::SaveProfilePage::Tab::kSync
                                                   : carview::SaveProfilePage::Tab::kBackups);
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  settings_page.switch_tab(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                  settings_page.step(-1);
                  apply_live();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                  settings_page.step(1);
                  apply_live();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (!settings_page.capturing_key()) {
                if (e.type == SDL_EVENT_MOUSE_MOTION) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.motion.x, e.motion.y, hz) &&
                      (hz.group == carview::ui::kHitRow || hz.group == carview::ui::kHitStepLeft ||
                       hz.group == carview::ui::kHitStepRight) &&
                      hz.index >= 0 && settings_page.selected() != hz.index) {
                    settings_page.select_row(hz.index);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                  if (e.button.button == SDL_BUTTON_RIGHT) {
                    settings_page.close();
                    settings_live_overlay = false;
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if (e.button.button == SDL_BUTTON_LEFT) {
                    carview::ui::HitZone hz{};
                    if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                      if (hz.group == carview::ui::kHitTab) {
                        settings_page.select_tab(hz.index);
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.group == carview::ui::kHitStepLeft) {
                        settings_page.select_row(hz.index);
                        settings_page.step(-1);
                        apply_live();
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.group == carview::ui::kHitStepRight) {
                        settings_page.select_row(hz.index);
                        settings_page.step(1);
                        apply_live();
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.group == carview::ui::kHitRow) {
                        settings_page.select_row(hz.index);
                        if (settings_page.on_rebind_row() || settings_page.on_save_line()) {
                          app.audio_host.trigger_ui(audio::UiCue::kAccept);
                          confirm();
                        } else {
                          settings_page.step(1);
                          apply_live();
                          app.audio_host.trigger_ui(audio::UiCue::kMove);
                        }
                      }
                    }
                  }
                }
                if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                  if (e.wheel.y > 0) settings_page.move(-1);
                  else if (e.wheel.y < 0) settings_page.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The career page & sponsor challenges board: supports keyboard, gamepad, and full mouse UX.
            if (career_page.is_open()) {
              const int n_sponsors = app.have_skills_data ? int(app.skills_data.sponsor_challenges.size()) : 0;
              auto close_or_back = [&]() {
                if (career_page.show_sponsors()) {
                  career_page.set_show_sponsors(false);
                } else {
                  career_page.close();
                }
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) close_or_back();
                else if ((k == SDLK_TAB || k == SDLK_Q || k == SDLK_E) && !e.key.repeat && n_sponsors > 0) {
                  career_page.toggle_sponsors();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_UP || k == SDLK_W) {
                  if (career_page.show_sponsors()) career_page.move_sponsor(-1, n_sponsors);
                  else career_page.move(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_DOWN || k == SDLK_S) {
                  if (career_page.show_sponsors()) career_page.move_sponsor(1, n_sponsors);
                  else career_page.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) close_or_back();
                else if ((b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) &&
                         n_sponsors > 0) {
                  career_page.toggle_sponsors();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  if (career_page.show_sponsors()) career_page.move_sponsor(-1, n_sponsors);
                  else career_page.move(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  if (career_page.show_sponsors()) career_page.move_sponsor(1, n_sponsors);
                  else career_page.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                  if (career_page.show_sponsors()) {
                    if (career_page.sponsor_selected() != hz.index && hz.index < n_sponsors) {
                      career_page.select_sponsor(hz.index, n_sponsors);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    }
                  } else if (career_page.visible_selected() != hz.index) {
                    career_page.select_visible(hz.index);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  close_or_back();
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                      if (career_page.show_sponsors()) career_page.select_sponsor(hz.index, n_sponsors);
                      else career_page.select_visible(hz.index);
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (career_page.show_sponsors() && hz.index == 1) close_or_back();
                      else if (!career_page.show_sponsors() && hz.index >= 1) close_or_back();
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                const int dir = e.wheel.y > 0 ? -1 : +1;
                if (career_page.show_sponsors()) career_page.move_sponsor(dir, n_sponsors);
                else career_page.move(dir);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The pause menu takes the keys and buttons while open.
            if (menu.open) {
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) {
                  menu.open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                if (k == SDLK_UP || k == SDLK_W) {
                  menu.move(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (k == SDLK_DOWN || k == SDLK_S) {
                  menu.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && (k == SDLK_LEFT || k == SDLK_A)) {
                  cycle_difficulty(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && (k == SDLK_RIGHT || k == SDLK_D)) {
                  cycle_difficulty(+1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  choose();
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  menu.open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  menu.move(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  menu.move(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                  cycle_difficulty(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (menu.selected_action() == PauseMenu::kDifficulty && b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                  cycle_difficulty(+1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  choose();
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                const int n_menu = int(menu.items().size());
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow &&
                    hz.index >= 0 && hz.index < n_menu && menu.selected != hz.index) {
                  menu.selected = hz.index;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  menu.open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  const int n_menu = int(menu.items().size());
                  if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui::kHitRow &&
                      hz.index >= 0 && hz.index < n_menu) {
                    menu.selected = hz.index;
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    choose();
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) menu.move(-1);
                else if (e.wheel.y < 0) menu.move(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (achievements.screen_open()) {
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if ((k == SDLK_ESCAPE || k == SDLK_F7) && !e.key.repeat) {
                  achievements.close_screen();
                  achievements.save_state(achievements_path);
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_LEFT || k == SDLK_A) {
                  achievements.move_focus(-1, 0);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_RIGHT || k == SDLK_D) {
                  achievements.move_focus(+1, 0);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_UP || k == SDLK_W) {
                  achievements.move_focus(0, -1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_DOWN || k == SDLK_S) {
                  achievements.move_focus(0, +1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_Q) {
                  achievements.switch_tab(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_E || k == SDLK_TAB) {
                  achievements.switch_tab((e.key.mod & SDL_KMOD_SHIFT) ? -1 : +1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_R && !e.key.repeat) {
                  achievements.cycle_sort();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if ((k == SDLK_T || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  achievements.toggle_track_focused();
                  achievements.save_state(achievements_path);
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (k == SDLK_F8 && !e.key.repeat) {
                  pending_shot = PendingShot::kF8;
                  host.renderer.request_capture(true);
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) {
                  achievements.close_screen();
                  achievements.save_state(achievements_path);
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                  achievements.move_focus(-1, 0);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                  achievements.move_focus(+1, 0);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  achievements.move_focus(0, -1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  achievements.move_focus(0, +1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  achievements.switch_tab(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                  achievements.switch_tab(+1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  achievements.cycle_sort();
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  achievements.toggle_track_focused();
                  achievements.save_state(achievements_path);
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0 &&
                    achievements.focus_index() != hz.index) {
                  achievements.select_focus_index(hz.index);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  achievements.close_screen();
                  achievements.save_state(achievements_path);
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitTab && hz.index >= 0 &&
                        size_t(hz.index) < achievements.tabs().size()) {
                      achievements.select_tab(achievements.tabs()[size_t(hz.index)]);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                      achievements.select_focus_index(hz.index);
                      achievements.toggle_track_focused();
                      achievements.save_state(achievements_path);
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) {
                        achievements.toggle_track_focused();
                        achievements.save_state(achievements_path);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      } else if (hz.index == 1) {
                        achievements.switch_tab(+1);
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 2) {
                        achievements.cycle_sort();
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 3) {
                        achievements.close_screen();
                        achievements.save_state(achievements_path);
                        app.audio_host.trigger_ui(audio::UiCue::kCancel);
                      }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                achievements.move_focus(0, e.wheel.y > 0 ? -1 : +1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (photo_mode.active()) {
              auto snap_photo = [&]() {
                int cw = 0, ch = 0;
                SDL_GetWindowSizeInPixels(app.window, &cw, &ch);
                if (cw > 0 && ch > 0) {
                  photo_mode.begin_capture(uint32_t(cw), uint32_t(ch), 4);
                  pending_shot = PendingShot::kPhotoMode;
                  host.renderer.request_capture(false);
                  if (!app.race && app.pr_stunt_session.active()) pr_stunt_photo_pressed = true;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) photo_mode.exit();
                else if (k == SDLK_TAB && !e.key.repeat) photo_mode.toggle_effects();
                else if ((k == SDLK_H || k == SDLK_F2) && !e.key.repeat) photo_mode.toggle_ui_hidden();
                else if (k == SDLK_UP) photo_mode.move_row(-1);
                else if (k == SDLK_DOWN) photo_mode.move_row(+1);
                else if (k == SDLK_LEFT) photo_mode.adjust_row(-1);
                else if (k == SDLK_RIGHT) photo_mode.adjust_row(+1);
                else if (k == SDLK_R && !e.key.repeat) photo_mode.effects().reset();
                else if (k == SDLK_F && !e.key.repeat) {
                  const drive::Vec3 pat = vehicle.position();
                  photo_mode.autofocus({pat.x, pat.y + 0.65, pat.z});
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  snap_photo();
                } else if (k == SDLK_F8 && !e.key.repeat) {
                  pending_shot = PendingShot::kF8;
                  host.renderer.request_capture(!photo_mode.ui_hidden());
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) photo_mode.exit();
                else if (b == SDL_GAMEPAD_BUTTON_NORTH) photo_mode.toggle_effects();
                else if (b == SDL_GAMEPAD_BUTTON_BACK) photo_mode.toggle_ui_hidden();
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) photo_mode.move_row(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) photo_mode.move_row(+1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) photo_mode.adjust_row(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) photo_mode.adjust_row(+1);
                else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  const drive::Vec3 pat = vehicle.position();
                  photo_mode.autofocus({pat.x, pat.y + 0.65, pat.z});
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  snap_photo();
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) &&
                    (hz.group == carview::ui::kHitRow || hz.group == carview::ui::kHitStepLeft ||
                     hz.group == carview::ui::kHitStepRight) &&
                    hz.index >= 0 && photo_mode.selected_row() != hz.index) {
                  photo_mode.select_row(hz.index);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  photo_mode.exit();
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitStepLeft && hz.index >= 0) {
                      photo_mode.select_row(hz.index);
                      photo_mode.adjust_row(-1);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui::kHitStepRight && hz.index >= 0) {
                      photo_mode.select_row(hz.index);
                      photo_mode.adjust_row(+1);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                      photo_mode.select_row(hz.index);
                      photo_mode.adjust_row(+1);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) { app.audio_host.trigger_ui(audio::UiCue::kAccept); snap_photo(); }
                      else if (hz.index == 1) { photo_mode.toggle_effects(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                      else if (hz.index == 3) {
                        const drive::Vec3 pat = vehicle.position();
                        photo_mode.autofocus({pat.x, pat.y + 0.65, pat.z});
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      } else if (hz.index == 4) {
                        photo_mode.effects().reset();
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 5) {
                        photo_mode.exit();
                        app.audio_host.trigger_ui(audio::UiCue::kCancel);
                      }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (photo_mode.effects_open()) photo_mode.move_row(e.wheel.y > 0 ? -1 : +1);
                else photo_mode.camera().set_fov_deg(photo_mode.camera().fov_deg() - double(e.wheel.y) * 2.5);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if ((replay_viewer.active() || replay_viewer.browser_open()) && !photo_mode.active()) {
              auto enter_replay_photo = [&]() {
                replay_viewer.set_paused(true);
                if (const auto* fc = replay_viewer.focused_car()) {
                  const drive::Vec3 pf = drive::rotate({fc->quat[1], fc->quat[2], fc->quat[3], fc->quat[0]}, {0, 0, 1});
                  photo_mode.enter({double(fc->pos[0]), double(fc->pos[1]), double(fc->pos[2])},
                                   std::atan2(pf.x, pf.z),
                                   carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
                }
              };
              if (replay_viewer.browser_open()) {
                if (e.type == SDL_EVENT_KEY_DOWN) {
                  const SDL_Keycode k = e.key.key;
                  if (k == SDLK_ESCAPE) replay_viewer.close_browser();
                  else if (k == SDLK_UP || k == SDLK_W) replay_viewer.browser_move(-1);
                  else if (k == SDLK_DOWN || k == SDLK_S) replay_viewer.browser_move(+1);
                  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                    std::string lerr;
                    if (!replay_viewer.browser_load_selected(&lerr) && !lerr.empty())
                      replay_viewer.set_notice("LOAD FAILED: " + lerr);
                  }
                }
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                  const auto b = e.gbutton.button;
                  if (b == SDL_GAMEPAD_BUTTON_EAST) replay_viewer.close_browser();
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) replay_viewer.browser_move(-1);
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) replay_viewer.browser_move(+1);
                  else if (b == SDL_GAMEPAD_BUTTON_SOUTH) replay_viewer.browser_load_selected();
                }
                if (e.type == SDL_EVENT_MOUSE_MOTION) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0 &&
                      replay_viewer.browser_selected() != hz.index) {
                    replay_viewer.browser_select(hz.index);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                  if (e.button.button == SDL_BUTTON_RIGHT) {
                    replay_viewer.close_browser();
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if (e.button.button == SDL_BUTTON_LEFT) {
                    carview::ui::HitZone hz{};
                    if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                      if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                        replay_viewer.browser_select(hz.index);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        std::string lerr;
                        if (!replay_viewer.browser_load_selected(&lerr) && !lerr.empty())
                          replay_viewer.set_notice("LOAD FAILED: " + lerr);
                      } else if (hz.group == carview::ui4::kHitButton) {
                        if (hz.index == 0) {
                          app.audio_host.trigger_ui(audio::UiCue::kAccept);
                          replay_viewer.browser_load_selected();
                        } else if (hz.index == 1) {
                          replay_viewer.close_browser();
                          app.audio_host.trigger_ui(audio::UiCue::kCancel);
                        }
                      }
                    }
                  }
                }
                if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                  replay_viewer.browser_move(e.wheel.y > 0 ? -1 : +1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              } else {
                if (e.type == SDL_EVENT_KEY_DOWN) {
                  const SDL_Keycode k = e.key.key;
                  if (k == SDLK_ESCAPE) replay_viewer.close();
                  else if (k == SDLK_SPACE && !e.key.repeat) replay_viewer.toggle_pause();
                  else if (k == SDLK_LEFT) {
                    if (replay_viewer.paused()) replay_viewer.step_frame(-1);
                    else replay_viewer.scrub(-1.5);
                  } else if (k == SDLK_RIGHT) {
                    if (replay_viewer.paused()) replay_viewer.step_frame(+1);
                    else replay_viewer.scrub(+1.5);
                  } else if (k == SDLK_LEFTBRACKET) replay_viewer.step_speed(-1);
                  else if (k == SDLK_RIGHTBRACKET) replay_viewer.step_speed(+1);
                  else if (k == SDLK_M && !e.key.repeat) replay_viewer.set_slow_motion();
                  else if (k == SDLK_C && !e.key.repeat) replay_viewer.cycle_camera(+1);
                  else if (k == SDLK_V && !e.key.repeat) replay_viewer.cycle_focus_car(+1);
                  else if (k == SDLK_T && !e.key.repeat) replay_viewer.toggle_telemetry();
                  else if ((k == SDLK_H || k == SDLK_F2) && !e.key.repeat) replay_viewer.toggle_ui_hidden();
                  else if (k == SDLK_S && !e.key.repeat) {
                    std::filesystem::path saved_r;
                    std::string serr;
                    if (!replay_viewer.save_to_dir(replays_dir, saved_r, &serr))
                      replay_viewer.set_notice("SAVE FAILED: " + serr);
                  } else if (k == SDLK_L && !e.key.repeat) {
                    replay_viewer.open_browser(replays_dir);
                  } else if (k == SDLK_P && !e.key.repeat) {
                    enter_replay_photo();
                  } else if (k == SDLK_F8 && !e.key.repeat) {
                    pending_shot = PendingShot::kF8;
                    host.renderer.request_capture(!replay_viewer.ui_hidden());
                  }
                }
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                  const auto b = e.gbutton.button;
                  if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) replay_viewer.close();
                  else if (b == SDL_GAMEPAD_BUTTON_SOUTH) replay_viewer.toggle_pause();
                  else if (b == SDL_GAMEPAD_BUTTON_WEST) replay_viewer.cycle_camera(+1);
                  else if (b == SDL_GAMEPAD_BUTTON_NORTH) replay_viewer.toggle_telemetry();
                  else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) replay_viewer.trigger_rewind();
                  else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) replay_viewer.trigger_fast_forward();
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) replay_viewer.scrub(-2.0);
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) replay_viewer.scrub(+2.0);
                  else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) replay_viewer.set_slow_motion();
                  else if (b == SDL_GAMEPAD_BUTTON_BACK) replay_viewer.toggle_ui_hidden();
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                  if (e.button.button == SDL_BUTTON_RIGHT) {
                    replay_viewer.close();
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if (e.button.button == SDL_BUTTON_LEFT) {
                    carview::ui::HitZone hz{};
                    if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                      if (hz.group == carview::ui::kHitRow) {
                        if (hz.index == 0) replay_viewer.trigger_rewind();
                        else if (hz.index == 1) { replay_viewer.set_paused(false); }
                        else if (hz.index == 2) { replay_viewer.set_paused(true); }
                        else if (hz.index == 3) replay_viewer.trigger_fast_forward();
                        else if (hz.index == 4) replay_viewer.cycle_camera(+1);
                        else if (hz.index == 5) replay_viewer.toggle_telemetry();
                        else if (hz.index == 6) replay_viewer.seek_fraction(0.0);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      } else if (hz.group == carview::ui4::kHitTimeline) {
                        int win_w = 0, win_h = 0;
                        if (app.window) SDL_GetWindowSize(app.window, &win_w, &win_h);
                        const float sx = (win_w > 0 && last_ui_w > 0) ? (float(last_ui_w) / float(win_w)) : 1.0f;
                        const float mx_ui = e.button.x * sx;
                        const float span = std::max(1.0f, hz.x1 - hz.x0);
                        replay_viewer.seek_fraction(double(mx_ui - hz.x0) / double(span));
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.group == carview::ui4::kHitButton) {
                        if (hz.index == 0) { replay_viewer.toggle_pause(); app.audio_host.trigger_ui(audio::UiCue::kAccept); }
                        else if (hz.index == 2) { replay_viewer.cycle_camera(+1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                        else if (hz.index == 3) { replay_viewer.toggle_telemetry(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                        else if (hz.index == 4) { enter_replay_photo(); app.audio_host.trigger_ui(audio::UiCue::kAccept); }
                        else if (hz.index == 5) { replay_viewer.close(); app.audio_host.trigger_ui(audio::UiCue::kCancel); }
                      }
                    }
                  }
                }
                if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                  if (replay_viewer.paused()) replay_viewer.step_frame(e.wheel.y > 0 ? +1 : -1);
                  else replay_viewer.scrub(e.wheel.y > 0 ? +1.5 : -1.5);
                }
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (rewind_buf.active()) {
              auto restore_camera = [&](const drive::RewindCameraState& cs) {
                if (have_rig && cs.rig.has_value()) rig = *cs.rig;
                if (cs.has_view) last_cam_view = cs.view, have_cam_view = true;
                app.front = cs.front_view;
                rig_accel.reset();
              };
              auto do_resume = [&]() {
                drive::RewindCameraState cs;
                double restored_race_clock = 0.0;
                uint64_t snap_id = 0u;
                if (rewind_buf.resume(vehicle, &cs, nullptr, &restored_race_clock, nullptr, nullptr, &snap_id)) {
                  host.particle_sys.clear();
                  restore_world_rewind(snap_id, true);
                  host.skid_sys.break_all();
                  restore_camera(cs);
                  drawn_before = vehicle.pose();
                  accumulator = 0.0;
                }
              };
              auto do_cancel = [&]() {
                drive::RewindCameraState cs;
                uint64_t snap_id = 0u;
                if (rewind_buf.cancel(vehicle, &cs, nullptr, nullptr, nullptr, nullptr, &snap_id)) {
                  restore_world_rewind(snap_id, false);
                  host.skid_sys.break_all();
                  restore_camera(cs);
                  drawn_before = vehicle.pose();
                  accumulator = 0.0;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (rewind_buf.is_paused_at_keyframe() &&
                    (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE))
                  do_resume();
                else if (rewind_buf.is_paused_at_keyframe() && (k == SDLK_BACKSPACE || k == SDLK_Y))
                  rewind_buf.rewind_further();
                else if (rewind_buf.is_paused_at_keyframe() && (k == SDLK_V || k == SDLK_X))
                  rewind_buf.request_view_replay();
                else if (k == SDLK_ESCAPE || k == SDLK_B)
                  do_cancel();
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (rewind_buf.is_paused_at_keyframe() && b == SDL_GAMEPAD_BUTTON_SOUTH)
                  do_resume();
                else if (rewind_buf.is_paused_at_keyframe() &&
                         (b == SDL_GAMEPAD_BUTTON_NORTH ||
                          (rewind_buf.rewind_on_back() && b == SDL_GAMEPAD_BUTTON_BACK)))
                  rewind_buf.rewind_further();
                else if (rewind_buf.is_paused_at_keyframe() && b == SDL_GAMEPAD_BUTTON_WEST)
                  rewind_buf.request_view_replay();
                else if (b == SDL_GAMEPAD_BUTTON_EAST)
                  do_cancel();
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  do_cancel();
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui4::kHitButton) {
                    const auto act = carview::rewind_overlay_action_for_button(
                        rewind_buf.is_rewinding(), rewind_buf.is_paused_at_keyframe(), rewind_buf.at_oldest_slot(),
                        hz.index);
                    if (act == carview::RewindOverlayAction::kResume) {
                      do_resume();
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    } else if (act == carview::RewindOverlayAction::kRewindFurther) {
                      rewind_buf.rewind_further();
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (act == carview::RewindOverlayAction::kViewReplay) {
                      rewind_buf.request_view_replay();
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    } else if (act == carview::RewindOverlayAction::kCancel) {
                      do_cancel();
                      app.audio_host.trigger_ui(audio::UiCue::kCancel);
                    }
                  }
                }
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
  return false;
}

}  // namespace carview
