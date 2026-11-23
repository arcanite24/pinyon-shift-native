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

void DriveSession::persist_slice12_settings() {
              const auto sfile = settings_path();
              if (sfile.empty()) return;
              std::vector<std::string> toks = read_settings(sfile, nullptr);
              auto fmt_num = [](double v) {
                char b[32];
                std::snprintf(b, sizeof b, "%.2f", v);
                return std::string(b);
              };
              const auto& ce = slice12.cam_editor_cfg;
              toks.push_back("--chase-height");
              toks.push_back(fmt_num(ce.chase_height_m));
              toks.push_back("--chase-distance");
              toks.push_back(fmt_num(ce.chase_distance_m));
              toks.push_back("--chase-fov");
              toks.push_back(fmt_num(ce.chase_fov_scale));
              toks.push_back("--chase-stiffness");
              toks.push_back(fmt_num(ce.chase_stiffness));
              toks.push_back("--driving-view");
              toks.push_back(ce.driving_view == drive::DrivingView::kWheel
                                 ? "wheel"
                                 : (ce.driving_view == drive::DrivingView::kReplay ? "replay" : "normal"));
              const auto& im = slice12.imm_cam_cfg;
              toks.push_back("--immersive-gforce");
              toks.push_back(fmt_num(im.gforce));
              toks.push_back("--immersive-lean");
              toks.push_back(fmt_num(im.lean));
              toks.push_back("--immersive-shake");
              toks.push_back(fmt_num(im.shake));
              toks.push_back("--immersive-head");
              toks.push_back(fmt_num(im.head));
              const auto& ht = slice12.ht_cfg;
              toks.push_back(ht.enabled ? "--head-tracking" : "--no-head-tracking");
              toks.push_back("--head-tracking-port");
              toks.push_back(std::to_string(ht.port));
              toks.push_back("--head-tracking-invert");
              toks.push_back(std::string(ht.invert[0] ? "1" : "0") + (ht.invert[1] ? "1" : "0") +
                             (ht.invert[2] ? "1" : "0"));
              if (slice12.trainer.config.saved_map_road.has_value()) {
                toks.push_back("--trainer-saved-road");
                toks.push_back(drive::format_road_point(*slice12.trainer.config.saved_map_road));
              }
              write_settings(sfile, toks, nullptr);
}

bool DriveSession::handle_dev_overlays_event(const SDL_Event& e) {
            if (slice12.trainer_overlay.is_open()) {
              bool handled = false;
              const carview::Nav nv = carview::nav_from_sdl(e);
              const bool close_k = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F10);
              const bool tab_l = (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Q) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
              const bool tab_r = (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_E || e.key.key == SDLK_TAB)) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
              if (close_k || nv == carview::Nav::kBack) {
                slice12.trainer_overlay.close();
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
                handled = true;
              } else if (tab_l) {
                slice12.trainer_overlay.switch_tab(-1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (tab_r) {
                slice12.trainer_overlay.switch_tab(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kUp) {
                slice12.trainer_overlay.move(-1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kDown) {
                slice12.trainer_overlay.move(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kLeft) {
                slice12.trainer_overlay.step(-1, slice12.trainer);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kRight) {
                slice12.trainer_overlay.step(1, slice12.trainer);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                handled = true;
              } else if (nv == carview::Nav::kAccept) {
                const auto res = slice12.trainer_overlay.activate(
                    slice12.trainer, app.career.credits, app.career.wristband_points, app.collected_wristband,
                    double(SDL_GetTicksNS()) * 1e-9, bool(app.race));
                if (res.credits_changed || res.wristband_changed) save_due = true;
                if (res.saved_road_changed) persist_slice12_settings();
                if (!res.message.empty()) notice = res.message, notice_until = vehicle.time() + 3.0;
                if (res.close_overlay) slice12.trainer_overlay.close();
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
                handled = true;
              }
              if (handled) {
                // Keep FFB trace / telemetry probe synced live when toggled in Trainer Debug tab.
                const auto log_dir = settings_path().parent_path();
                if (slice12.trainer.config.debug_ffb_trace && !slice12.ffb_trace.is_recording())
                  slice12.ffb_trace.start(log_dir);
                else if (!slice12.trainer.config.debug_ffb_trace && slice12.ffb_trace.is_recording())
                  slice12.ffb_trace.stop();
                if (slice12.trainer.config.debug_telemetry_probe && !slice12.tel_probe.is_recording())
                  slice12.tel_probe.start(log_dir / "telemetry-probe.csv");
                else if (!slice12.trainer.config.debug_telemetry_probe && slice12.tel_probe.is_recording())
                  slice12.tel_probe.stop();
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                  e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED)
                return true;
            }
            if (slice12.cam_editor_overlay.is_open()) {
              bool changed = false;
              const carview::Nav nv = carview::nav_from_sdl(e);
              const bool close_k = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F6);
              const bool tab_l = (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Q) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
              const bool tab_r = (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_E || e.key.key == SDLK_TAB)) ||
                                 (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
              if (close_k || nv == carview::Nav::kBack) {
                slice12.cam_editor_overlay.close();
                persist_slice12_settings();
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
              } else if (tab_l) {
                slice12.cam_editor_overlay.switch_tab(-1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (tab_r) {
                slice12.cam_editor_overlay.switch_tab(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (nv == carview::Nav::kUp) {
                slice12.cam_editor_overlay.move(-1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (nv == carview::Nav::kDown) {
                slice12.cam_editor_overlay.move(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              } else if (nv == carview::Nav::kLeft) {
                slice12.cam_editor_overlay.step(-1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                changed = true;
              } else if (nv == carview::Nav::kRight) {
                slice12.cam_editor_overlay.step(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                changed = true;
              } else if (nv == carview::Nav::kAccept) {
                slice12.cam_editor_overlay.activate();
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
                changed = true;
              }
              if (changed) {
                slice12.cam_editor_cfg = slice12.cam_editor_overlay.editor_config();
                slice12.imm_cam_cfg = slice12.cam_editor_overlay.immersive_config();
                slice12.ht_cfg = slice12.cam_editor_overlay.head_tracking_config();
                rig.set_camera_editor(slice12.cam_editor_cfg);
                rig.set_immersive_camera(slice12.imm_cam_cfg);
                slice12.ht_receiver.configure(slice12.ht_cfg);
              }
              if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                  e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED)
                return true;
            }
            // F6 > MODS (mods_page.h): the overlay owns the input while open.
            if (mods_page.is_open()) {
              const carview::Nav nv = carview::nav_from_sdl(e);
              if (nv != carview::Nav::kNone) {
                app.audio_host.trigger_ui(nv == carview::Nav::kBack ? audio::UiCue::kCancel : audio::UiCue::kMove);
                if (mods_page.nav(nv)) reload_mod_textures();
              } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0 &&
                    mods_page.selected() != hz.index) {
                  mods_page.select_row(hz.index);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  mods_page.nav(carview::Nav::kBack);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                      mods_page.select_row(hz.index);
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      if (mods_page.nav(carview::Nav::kAccept)) reload_mod_textures();
                    } else if (hz.group == carview::ui4::kHitButton) {
                      carview::Nav bn = carview::Nav::kNone;
                      if (hz.index == 0) bn = carview::Nav::kAccept;
                      else if (hz.index == 1) bn = carview::Nav::kTabPrev;
                      else if (hz.index == 2) bn = carview::Nav::kTabNext;
                      else if (hz.index == 3) bn = carview::Nav::kRight;
                      else if (hz.index == 4) bn = carview::Nav::kX;
                      else if (hz.index == 5) bn = carview::Nav::kY;
                      else if (hz.index == 6) bn = carview::Nav::kBack;
                      if (bn != carview::Nav::kNone) {
                        app.audio_host.trigger_ui(bn == carview::Nav::kBack ? audio::UiCue::kCancel : audio::UiCue::kAccept);
                        if (mods_page.nav(bn)) reload_mod_textures();
                      }
                    }
                  }
                }
              } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                mods_page.nav(e.wheel.y > 0 ? carview::Nav::kUp : carview::Nav::kDown);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // Profile > Save Backups & Save Sync (save_sync_ui.h).
            if (save_page.is_open()) {
              if (save_page.osk_open() && e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE) {
                save_page.backspace();
              } else if (save_page.osk_open() && e.type == SDL_EVENT_TEXT_INPUT && e.text.text && e.text.text[0]) {
                for (const char* p = e.text.text; *p; ++p) save_page.type_char(*p);
              } else if (save_page.osk_open() && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                         e.button.button == SDL_BUTTON_RIGHT) {
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
                save_page.nav(carview::Nav::kBack, app.save_session);
              } else {
                const carview::Nav nv = carview::nav_from_sdl(e);
                if (nv != carview::Nav::kNone) {
                  app.audio_host.trigger_ui(nv == carview::Nav::kBack ? audio::UiCue::kCancel : audio::UiCue::kMove);
                  save_page.nav(nv, app.save_session);
                } else if (!save_page.osk_open() && e.type == SDL_EVENT_MOUSE_MOTION) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0 &&
                      save_page.selected() != hz.index) {
                    save_page.select_row(hz.index, app.save_session);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                } else if (!save_page.osk_open() && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                  if (e.button.button == SDL_BUTTON_RIGHT) {
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                    save_page.nav(carview::Nav::kBack, app.save_session);
                  } else if (e.button.button == SDL_BUTTON_LEFT) {
                    carview::ui::HitZone hz{};
                    if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                      if (hz.group == carview::ui::kHitTab && hz.index >= 0 &&
                          hz.index < int(carview::SaveProfilePage::Tab::kCount)) {
                        save_page.set_tab(carview::SaveProfilePage::Tab(hz.index));
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                        save_page.select_row(hz.index, app.save_session);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        save_page.nav(carview::Nav::kAccept, app.save_session);
                      } else if (hz.group == carview::ui4::kHitButton) {
                        const auto t = save_page.tab();
                        carview::Nav bn = carview::Nav::kNone;
                        if (t == carview::SaveProfilePage::Tab::kBackups) {
                          if (hz.index == 0) bn = carview::Nav::kAccept;
                          else if (hz.index == 1) bn = carview::Nav::kY;
                          else if (hz.index == 2) bn = carview::Nav::kTabPrev;
                          else if (hz.index == 3) bn = carview::Nav::kTabNext;
                          else if (hz.index == 4) bn = carview::Nav::kBack;
                        } else if (t == carview::SaveProfilePage::Tab::kSync ||
                                   t == carview::SaveProfilePage::Tab::kProfiles) {
                          if (hz.index == 0) bn = carview::Nav::kAccept;
                          else if (hz.index == 1) bn = carview::Nav::kX;
                          else if (hz.index == 2) bn = carview::Nav::kY;
                          else if (hz.index == 3) bn = carview::Nav::kTabPrev;
                          else if (hz.index == 4) bn = carview::Nav::kTabNext;
                          else if (hz.index == 5) bn = carview::Nav::kBack;
                        } else {
                          if (hz.index == 0) bn = carview::Nav::kAccept;
                          else if (hz.index == 1) bn = carview::Nav::kX;
                          else if (hz.index == 2) bn = carview::Nav::kTabPrev;
                          else if (hz.index == 3) bn = carview::Nav::kTabNext;
                          else if (hz.index == 4) bn = carview::Nav::kBack;
                        }
                        if (bn != carview::Nav::kNone) {
                          app.audio_host.trigger_ui(bn == carview::Nav::kBack ? audio::UiCue::kCancel
                                                                          : audio::UiCue::kAccept);
                          save_page.nav(bn, app.save_session);
                        }
                      }
                    }
                  }
                } else if (!save_page.osk_open() && e.type == SDL_EVENT_MOUSE_WHEEL) {
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                  save_page.nav(e.wheel.y > 0 ? carview::Nav::kUp : carview::Nav::kDown, app.save_session);
                }
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The settings page: up and down pick a row, left and right
            // change it (applied live), enter rebinds a key or saves.
  return false;
}

}  // namespace carview
