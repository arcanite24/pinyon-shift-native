#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "cars/livery.h"
#include "cars/sources.h"
#include "garage/rally_kit.h"
#include "settings.h"

namespace carview {

namespace {

bool hit_test_osk_key_hub(SDL_Window* win, uint32_t fb_w, uint32_t fb_h, float mx, float my, bool symbols_mode,
                          int& out_row, int& out_col) {
  const float sw = float(fb_w), sh = float(fb_h);
  if (sw < 64.0f || sh < 64.0f) return false;
  float ux = mx, uy = my;
  if (win) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    if (ww > 0 && wh > 0) {
      ux = mx * sw / float(ww);
      uy = my * sh / float(wh);
    }
  }
  const float s = sh / 720.0f;
  const float pw = 760.0f * s, ph = 428.0f * s;
  const float px0 = 0.5f * (sw - pw), py0 = 0.5f * (sh - ph) - 10.0f * s;
  const float cx = px0 + 0.5f * pw, cy = py0 + 0.5f * ph;
  const float ang = -carview::ui4::kTiltRad;
  const float cs = std::cos(ang), sn = std::sin(ang);
  const float dx = ux - cx, dy = uy - cy;
  const float lx = cx + cs * dx - sn * dy;
  const float ly = cy + sn * dx + cs * dy;

  const float gx0 = px0 + 24.0f * s, gw = pw - 48.0f * s;
  const float gy0 = py0 + 114.0f * s;
  const float gap = 6.0f * s;
  const float key_h = 46.0f * s;
  static constexpr int kSymCols[4] = {10, 10, 10, 5};
  static constexpr int kAlphaCols[5] = {10, 10, 10, 10, 5};
  const int* col_counts = symbols_mode ? kSymCols : kAlphaCols;
  const int num_rows = symbols_mode ? 4 : 5;
  for (int r = 0; r < num_rows; ++r) {
    const float ky0 = gy0 + float(r) * (key_h + gap);
    if (ly < ky0 || ly > ky0 + key_h) continue;
    const int ncols = col_counts[r];
    const float kw = (gw - gap * float(ncols - 1)) / float(ncols);
    for (int c = 0; c < ncols; ++c) {
      const float kx0 = gx0 + float(c) * (kw + gap);
      if (lx >= kx0 && lx <= kx0 + kw) {
        out_row = r;
        out_col = c;
        return true;
      }
    }
  }
  return false;
}

bool reapply_active_car_livery(DriveApp& app, DriveWorldHost& host) {
  if (!app.have_garage) return false;
  const garage::Car* gc = app.career_garage.selected_car();
  if (!gc || gc->livery_file.empty()) return false;
  auto it = app.baked_ids.find(gc->car_id);
  if (it == app.baked_ids.end() || lower_ascii(it->second) != lower_ascii(app.car)) return false;
  std::filesystem::path liv_p(gc->livery_file);
  if (liv_p.is_relative() && !app.save_path.empty()) liv_p = app.save_path.parent_path() / liv_p;
  cars::LiveryDocument ldoc;
  if (!cars::load_livery_file(liv_p, ldoc, nullptr)) return false;
  host.showroom.set_paint_spec(ldoc.paint);
  if (!ldoc.layers.empty()) {
    cars::LiveryLibrary llib;
    const auto gr = app.game_root_of();
    if (!gr.empty()) {
      const auto lzip = gr / "media" / "Livery.zip";
      if (std::filesystem::exists(lzip)) llib.open(lzip, nullptr);
    }
    std::vector<uint8_t> liv_rgba;
    if (cars::composite_livery(ldoc, llib.is_open() ? &llib : nullptr,
                               host.showroom.livery_planes().empty() ? nullptr : &host.showroom.livery_planes(),
                               liv_rgba, 1024)) {
      std::string lerr;
      host.showroom.set_custom_livery(liv_rgba.data(), 1024, 1024, lerr);
    }
  }
  return true;
}

bool handle_hub_orbit_input(DriveApp& app, HubCamera& hub_cam, const SDL_Event& e, bool hit_ui,
                            bool allow_right_stick) {
  if (allow_right_stick && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    const float v = float(e.gaxis.value) / 32767.0f;
    if (std::fabs(v) > 0.22f) {
      if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX) {
        hub_cam.nudge_orbit(v * 0.08f, 0.0f, 0.0f);
        return true;
      }
      if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY) {
        hub_cam.nudge_orbit(0.0f, -v * 0.05f, 0.0f);
        return true;
      }
    }
  }
  if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE)) {
    if (e.button.button == SDL_BUTTON_MIDDLE || !hit_ui) {
      app.hub_orbit_dragging = true;
      app.hub_orbit_dragged = false;
      app.hub_orbit_last_x = e.button.x;
      app.hub_orbit_last_y = e.button.y;
      return true;
    }
  } else if (e.type == SDL_EVENT_MOUSE_MOTION && app.hub_orbit_dragging) {
    const float dx = e.motion.x - app.hub_orbit_last_x;
    const float dy = e.motion.y - app.hub_orbit_last_y;
    app.hub_orbit_last_x = e.motion.x;
    app.hub_orbit_last_y = e.motion.y;
    if (std::fabs(dx) > 0.5f || std::fabs(dy) > 0.5f) {
      app.hub_orbit_dragged = true;
      hub_cam.nudge_orbit(dx * 0.0085f, dy * 0.0060f, 0.0f);
      return true;
    }
  } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
             (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE)) {
    if (app.hub_orbit_dragging) {
      app.hub_orbit_dragging = false;
      return app.hub_orbit_dragged;
    }
  }
  return false;
}

int autoshow_swatch_hit_index(const DriveApp& app, const Autoshow& autoshow, int last_ui_w, int last_ui_h, float mx,
                              float my) {
  const AutoshowCar* sel_car = autoshow.selected_car();
  if (!sel_car || sel_car->swatches.empty()) return -1;
  int win_w = 0, win_h = 0;
  if (app.window) SDL_GetWindowSize(app.window, &win_w, &win_h);
  const float sx_scale = (win_w > 0 && last_ui_w > 0) ? (float(last_ui_w) / float(win_w)) : 1.0f;
  const float sy_scale = (win_h > 0 && last_ui_h > 0) ? (float(last_ui_h) / float(win_h)) : 1.0f;
  const float ux = mx * sx_scale;
  const float uy = my * sy_scale;
  const float w = float(std::max(1, last_ui_w));
  const float h = float(std::max(1, last_ui_h));
  const AutoshowLayout lo =
      compute_autoshow_layout(w, h, AutoshowStep::kColorSelect, autoshow.show_specs(), false);
  if (!lo.swatch_strip.valid() || uy < lo.swatch_strip.y0 || uy > lo.swatch_strip.y1) return -1;
  size_t first_sw = 0, nsw = 0;
  autoshow.swatch_window(14, first_sw, nsw);
  if (nsw == 0) return -1;
  const float s = lo.scale;
  const float pad_lr = 24.0f * s;
  const float sw_gap = 6.0f * s;
  const float avail_w = std::max(80.0f * s, lo.swatch_strip.width() - 2.0f * pad_lr);
  const float sw_w = std::min(48.0f * s, (avail_w - sw_gap * float(std::max<size_t>(1, nsw) - 1)) /
                                             float(std::max<size_t>(1, nsw)));
  float cx = lo.swatch_strip.x0 + pad_lr;
  for (size_t i = 0; i < nsw; ++i) {
    if (ux >= cx && ux <= cx + sw_w) return int(first_sw + i);
    cx += sw_w + sw_gap;
  }
  return -1;
}

bool any_pad_trigger_held(const std::vector<SDL_Gamepad*>& pads, SDL_GamepadAxis axis) {
  for (SDL_Gamepad* gp : pads) {
    if (gp && SDL_GetGamepadAxis(gp, axis) > 12000) return true;
  }
  return false;
}

}  // namespace

bool DriveSession::handle_hub_shop_event(const SDL_Event& e) {
            if (garage_open && garage_popup != GaragePopup::kNone) {
              // A popup over the list (H_MESSAGE_POPUP): A its answer, B
              // away, X VIEW ON MAP where it offers it.
              enum class Ans { kNone, kAccept, kBack, kMap } ans = Ans::kNone;
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) ans = Ans::kAccept;
                else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) ans = Ans::kBack;
                else if (k == SDLK_X || k == SDLK_M) ans = Ans::kMap;
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) ans = Ans::kAccept;
                else if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) ans = Ans::kBack;
                else if (b == SDL_GAMEPAD_BUTTON_WEST) ans = Ans::kMap;
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  ans = Ans::kBack;
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui4::kHitButton)
                    ans = hz.index == 0 ? Ans::kAccept : (garage_popup == GaragePopup::kCollect ? Ans::kBack : Ans::kMap);
                }
              }
              if (ans == Ans::kAccept) {
                garage_popup_accept();
              } else if (ans == Ans::kBack) {
                garage_popup = GaragePopup::kNone;
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
              } else if (ans == Ans::kMap &&
                         (garage_popup == GaragePopup::kNotReady || garage_popup == GaragePopup::kNotDiscovered)) {
                garage_popup_view_map();
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                      e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (garage_open && garage_barn_finds) {
              // BARN FINDS: every barn on 108; A on one opens its popup.
              const size_t n = app.collectibles.barns.size();
              auto close = [&]() {
                garage_open = false;
                garage_barn_finds = false;
                app.hub_orbit_dragging = false;
                app.hub_orbit_dragged = false;
                app.audio_host.trigger_ui(audio::UiCue::kCancel);
              };
              auto move = [&](int by) {
                if (!n) return;
                barn_pick = size_t(((int(barn_pick) + by) % int(n) + int(n)) % int(n));
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto toggle = [&]() {
                garage_show_specs = !garage_show_specs;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) close();
                else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) move(-1);
                else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) move(1);
                else if (k == SDLK_Y && !e.key.repeat) toggle();
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) barn_select();
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) close();
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) move(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) move(1);
                else if (b == SDL_GAMEPAD_BUTTON_NORTH) toggle();
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) barn_select();
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                carview::ui::HitZone hz{};
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  close();
                } else if (e.button.button == SDL_BUTTON_LEFT && ui_mouse_hit(e.button.x, e.button.y, hz)) {
                  if (hz.group == carview::ui::kHitRow && hz.index >= 0 && size_t(hz.index) < n) {
                    barn_pick = size_t(hz.index);
                    barn_select();
                  } else if (hz.group == carview::ui4::kHitButton) {
                    if (hz.index == 0) barn_select();
                    else if (hz.index == 1) close();
                    else if (hz.index == 2) toggle();
                  }
                }
              } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) move(-1);
                else if (e.wheel.y < 0) move(1);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                      e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (garage_open) {
              const bool use_browser = app.have_garage && !app.career_garage.cars.empty();
              const size_t n = use_browser ? garage_browser.visible().size() : app.baked_cars.size();
              auto move_garage = [&](int by) {
                garage_sell_confirm_id = -1;
                if (use_browser) {
                  if (n) garage_browser.move(by);
                } else if (n) {
                  garage_pick = size_t(((int(garage_pick) + by) % int(n) + int(n)) % int(n));
                }
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto drive_selected = [&]() {
                garage_sell_confirm_id = -1;
                if (use_browser) {
                  const auto* e = garage_browser.selected_entry();
                  if (!e) return;
                  garage_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  std::string target_media = e->media_name;
                  if (target_media.empty() && app.baked_ids.count(e->car_id)) target_media = app.baked_ids.at(e->car_id);
                  if (target_media.empty()) return;
                  app.career_garage.selected = e->garage_id;
                  app.write_career_garage();
                  const drive::Vec3 at = vehicle.position();
                  const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
                  app.spawn[0] = at.x;
                  app.spawn[1] = at.y;
                  app.spawn[2] = at.z;
                  app.spawn[3] = std::atan2(fwd.x, fwd.z);
                  app.have_spawn = true;
                  app.next_car = target_media;
                  running = false;
                  return;
                }
                if (!n || garage_pick >= app.baked_cars.size()) return;
                garage_open = false;
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
                pick = garage_pick;
                picking = true;
                take_pick();
              };
              auto try_sell = [&]() {
                if (!use_browser) return;
                const auto* se = garage_browser.selected_entry();
                if (!se) return;
                if (is_barn_car(se->car_id)) {
                  // CAN'T SELL BARN FIND: "You cannot sell Barn finds."
                  garage_sell_confirm_id = -1;
                  garage_popup = GaragePopup::kCantSell;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  return;
                }
                if (se->is_current) {
                  notice = "SWITCH CARS BEFORE SELLING YOUR ACTIVE CAR";
                  notice_until = vehicle.time() + 3.0;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  return;
                }
                if (app.career_garage.cars.size() <= 1) {
                  notice = "CANNOT SELL YOUR LAST CAR";
                  notice_until = vehicle.time() + 3.0;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  return;
                }
                if (garage_sell_confirm_id != se->garage_id) {
                  garage_sell_confirm_id = se->garage_id;
                  notice = "PRESS [LB] / DEL AGAIN TO SELL FOR +" + carview::thousands(se->sell_cr) + " Cr";
                  notice_until = vehicle.time() + 4.0;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                  return;
                }
                garage_sell_confirm_id = -1;
                const cars::PartsDb* gdb =
                    (app.parts_db.valid() && app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                        ? app.parts_db.get().get()
                        : nullptr;
                std::string smsg;
                if (garage_browser.sell_selected(app.career_garage, gdb, app.class_bands, app.career.credits, smsg)) {
                  app.garage_picker();
                  app.write_career_garage();
                  save_now("sell");
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                notice = smsg;
                notice_until = vehicle.time() + 3.5;
              };
              auto toggle_fav = [&]() {
                if (!use_browser) return;
                const cars::PartsDb* gdb =
                    (app.parts_db.valid() && app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                        ? app.parts_db.get().get()
                        : nullptr;
                const bool now_fav = garage_browser.toggle_favorite_selected(app.career_garage, gdb, app.class_bands);
                app.write_career_garage();
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
                notice = now_fav ? "ADDED TO FAVORITES" : "REMOVED FROM FAVORITES";
                notice_until = vehicle.time() + 2.5;
              };
              auto toggle_fav_filter = [&]() {
                if (!use_browser) return;
                garage_sell_confirm_id = -1;
                garage_browser.toggle_favorites_filter();
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                notice = garage_browser.favorites_only() ? "FILTER: FAVORITES ONLY" : "FILTER: ALL GARAGE CARS";
                notice_until = vehicle.time() + 2.5;
              };
              // Engine extra: the class filter (108 has no tab strip, so it says so).
              auto cycle_class = [&]() {
                garage_sell_confirm_id = -1;
                garage_browser.cycle_class_filter(1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
                const int cf = garage_browser.class_filter();
                notice = cf >= 0 && cf < int(app.class_bands.size())
                             ? "FILTER: CLASS " + app.class_bands[size_t(cf)].name
                             : std::string("FILTER: ALL CLASSES");
                notice_until = vehicle.time() + 2.5;
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) {
                  garage_open = false;
                  garage_sell_confirm_id = -1;
                  app.hub_orbit_dragging = false;
                  app.hub_orbit_dragged = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) {
                  move_garage(-1);
                } else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) {
                  move_garage(1);
                } else if (k == SDLK_Y && !e.key.repeat && use_browser) {
                  // Y TOGGLE: the car's details on and off (108's help bar).
                  garage_show_specs = !garage_show_specs;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_O && !e.key.repeat && use_browser) {
                  // Engine extra: the sorts.
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_sort(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                  notice = std::string("SORT: ") + carview::garage_sort_name(garage_browser.sort_mode());
                  notice_until = vehicle.time() + 2.5;
                } else if ((k == SDLK_X || k == SDLK_M) && !e.key.repeat && use_browser) {
                  cycle_class();  // M: SEARCH, as on the Autoshow's list
                } else if (k == SDLK_F && !e.key.repeat && use_browser) {
                  toggle_fav();
                } else if (k == SDLK_V && !e.key.repeat && use_browser) {
                  toggle_fav_filter();
                } else if ((k == SDLK_DELETE || k == SDLK_BACKSPACE || k == SDLK_Q) && !e.key.repeat) {
                  try_sell();
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  drive_selected();
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  garage_open = false;
                  garage_sell_confirm_id = -1;
                  app.hub_orbit_dragging = false;
                  app.hub_orbit_dragged = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  move_garage(-1);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  move_garage(1);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH && use_browser) {
                  garage_show_specs = !garage_show_specs;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_STICK && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_sort(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                  notice = std::string("SORT: ") + carview::garage_sort_name(garage_browser.sort_mode());
                  notice_until = vehicle.time() + 2.5;
                } else if ((b == SDL_GAMEPAD_BUTTON_WEST || b == SDL_GAMEPAD_BUTTON_BACK) && use_browser) {
                  cycle_class();
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER && use_browser) {
                  toggle_fav();
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_STICK && use_browser) {
                  toggle_fav_filter();
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  try_sell();
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  drive_selected();
                }
              }
              {
                carview::ui::HitZone orb_hz{};
                const bool on_ui = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) &&
                                   ui_mouse_hit(e.button.x, e.button.y, orb_hz);
                handle_hub_orbit_input(app, hub_cam, e, on_ui, true);
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION && !app.hub_orbit_dragging && n > 0) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow &&
                    hz.index >= 0 && size_t(hz.index) < n) {
                  if (use_browser) {
                    if (garage_browser.selected_index() != size_t(hz.index)) {
                      garage_sell_confirm_id = -1;
                      garage_browser.select_index(size_t(hz.index));
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    }
                  } else if (garage_pick != size_t(hz.index)) {
                    garage_pick = size_t(hz.index);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  garage_open = false;
                  garage_sell_confirm_id = -1;
                  app.hub_orbit_dragging = false;
                  app.hub_orbit_dragged = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT && n > 0) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && hz.index >= 0 && size_t(hz.index) < n) {
                      if (use_browser) garage_browser.select_index(size_t(hz.index));
                      else garage_pick = size_t(hz.index);
                      drive_selected();
                    } else if (hz.group == carview::ui4::kHitButton && car_list_drawn) {
                      // 108's help bar: SELECT, BACK, TOGGLE.
                      if (hz.index == 0) {
                        drive_selected();
                      } else if (hz.index == 1) {
                        garage_open = false;
                        garage_sell_confirm_id = -1;
                        app.hub_orbit_dragging = false;
                        app.hub_orbit_dragged = false;
                        app.audio_host.trigger_ui(audio::UiCue::kCancel);
                      } else if (hz.index == 2) {
                        garage_show_specs = !garage_show_specs;
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 3 && use_browser) {
                        cycle_class();
                      }
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) drive_selected();
                      else if (hz.index == 1 && use_browser) {
                        garage_sell_confirm_id = -1;
                        garage_browser.cycle_sort(1);
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 2 && use_browser) {
                        garage_sell_confirm_id = -1;
                        garage_browser.cycle_class_filter(1);
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 3 && use_browser) {
                        toggle_fav();
                      } else if (hz.index == 4) {
                        try_sell();
                      } else if (hz.index == 5) {
                        garage_open = false;
                        garage_sell_confirm_id = -1;
                        app.hub_orbit_dragging = false;
                        app.hub_orbit_dragged = false;
                        app.audio_host.trigger_ui(audio::UiCue::kCancel);
                      }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL && n > 0) {
                if (e.wheel.y > 0) move_garage(-1);
                else if (e.wheel.y < 0) move_garage(1);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The upgrade shop takes the keys and buttons while open; leaving it
            // after a change rebuilds the car where it stands.
            if (shop.is_open()) {
              // Retail FH1: upgrade discounts come from Discount Signs (0..100%) ONLY (never Affinity stacking).
              const int signs_disc = garage::upgrade_discount_percent(
                  host.free_roam ? host.free_roam->signs_found() : int(app.saved.signs_found.size()));
              shop.set_signs_discount_percent(signs_disc);
              auto sync_shop_preview = [&]() {
                if (!shop.visual_dirty()) return;
                cars::VisualSetup pv;
                if (!shop.preview_visual(pv)) {
                  shop.clear_visual_dirty();
                  return;
                }
                const auto& cur_v = host.showroom.options().visual;
                const bool visual_changed =
                    cur_v.wheel_media != pv.wheel_media || cur_v.rim_inches != pv.rim_inches ||
                    cur_v.tyre_width_mm != pv.tyre_width_mm || cur_v.shown_parts != pv.shown_parts ||
                    cur_v.removed_parts != pv.removed_parts || cur_v.exhaust_part != pv.exhaust_part ||
                    cur_v.tire_compound_level != pv.tire_compound_level ||
                    std::fabs(cur_v.ride_height_m[0] - pv.ride_height_m[0]) > 0.002f ||
                    std::fabs(cur_v.ride_height_m[1] - pv.ride_height_m[1]) > 0.002f ||
                    std::fabs(cur_v.camber_rad[0] - pv.camber_rad[0]) > 0.005f ||
                    std::fabs(cur_v.camber_rad[1] - pv.camber_rad[1]) > 0.005f ||
                    cur_v.brakes[0].level != pv.brakes[0].level || cur_v.brakes[1].level != pv.brakes[1].level;
                shop.clear_visual_dirty();
                if (!visual_changed) return;
                ShowroomOptions opts = host.showroom.options();
                opts.visual = std::move(pv);
                std::string rerr;
                host.showroom.reload_car(app.bake_dir, app.car, opts, rerr);
                reapply_active_car_livery(app, host);
              };
              auto leave_shop = [&]() {
                shop.close();
                app.hub_orbit_dragging = false;
                app.hub_orbit_dragged = false;
                const bool return_to_rally_service = app.rally_service_return;
                app.rally_service_return = false;
                if (!shop.changed()) {
                  // Restore fitted car appearance & custom livery if the player previewed un-bought parts.
                  ShowroomOptions orig = host.build_car_options();
                  std::string rerr;
                  host.showroom.reload_car(app.bake_dir, app.car, orig, rerr);
                  reapply_active_car_livery(app, host);
                  if (return_to_rally_service && app.rally_session.active() && app.rally_session.in_service_area()) {
                    app.rally_service_open = true;
                  }
                  return;
                }
                app.write_career_garage();
                save_now("upgrade");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
                app.have_spawn = true;
                app.next_car = app.car;
                if (return_to_rally_service && app.rally_session.active() && app.rally_session.in_service_area()) {
                  app.rally_service_resume_after_reload = true;
                }
                running = false;
              };
              auto fit = [&]() {
                std::string message;
                const bool ok = shop.fit(signs_disc, app.career.credits, message);
                app.audio_host.trigger_ui(ok ? audio::UiCue::kPurchase : audio::UiCue::kError);
                notice = message;
                notice_until = vehicle.time() + 3;
                sync_shop_preview();
              };
              auto step_shop_part = [&](int dir) {
                if (shop.category() == carview::ShopCategory::kTuning &&
                    shop.selected_slider() < shop.tuning_sliders().size()) {
                  const auto& sl = shop.tuning_sliders()[shop.selected_slider()];
                  if (!sl.unlocked) {
                    notice = sl.unlock_hint.empty() ? "LOCKED — FIT RACE PART TO UNLOCK" : sl.unlock_hint;
                    notice_until = vehicle.time() + 3.0;
                    app.audio_host.trigger_ui(audio::UiCue::kError);
                    return;
                  }
                }
                shop.move_part(dir);
                sync_shop_preview();
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto revert_na_action = [&]() {
                if (shop.category() == carview::ShopCategory::kConversions &&
                    shop.has_forced_induction_conversion_fitted()) {
                  std::string msg;
                  const bool ok = shop.revert_to_naturally_aspirated(msg);
                  app.audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kError);
                  notice = msg;
                  notice_until = vehicle.time() + 3.0;
                  sync_shop_preview();
                }
              };
              auto x_action = [&]() {
                if (shop.category() == carview::ShopCategory::kTuning) {
                  shop.reset_tuning();
                  app.write_career_garage();
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  notice = "TUNING RESET TO DEFAULT";
                  notice_until = vehicle.time() + 3.0;
                  sync_shop_preview();
                  return;
                }
                std::string smsg;
                const bool ok = shop.sell_shown_part(app.career.credits, smsg);
                app.audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kCancel);
                notice = smsg, notice_until = vehicle.time() + 3;
                sync_shop_preview();
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) { app.audio_host.trigger_ui(audio::UiCue::kCancel); leave_shop(); }
                if (k == SDLK_UP || k == SDLK_W) { shop.move_slot(-1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_DOWN || k == SDLK_S) { shop.move_slot(1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_LEFT || k == SDLK_A) { step_shop_part(-1); }
                if (k == SDLK_RIGHT || k == SDLK_D) { step_shop_part(1); }
                if (k == SDLK_Q) { shop.move_category(-1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_E) { shop.move_category(1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_R) { revert_na_action(); }
                if (k == SDLK_X) { x_action(); }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { fit(); }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  leave_shop();
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) { shop.move_slot(-1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { shop.move_slot(1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) { step_shop_part(-1); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) { step_shop_part(1); }
                if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { shop.move_category(-1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { shop.move_category(1); sync_shop_preview(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_BACK) { revert_na_action(); }
                if (b == SDL_GAMEPAD_BUTTON_WEST) { x_action(); }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) { fit(); }
              }
              {
                carview::ui::HitZone orb_hz{};
                const bool on_ui = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) &&
                                   ui_mouse_hit(e.button.x, e.button.y, orb_hz);
                handle_hub_orbit_input(app, hub_cam, e, on_ui, true);
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION && !app.hub_orbit_dragging) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                  const size_t cur_sel = shop.category() == carview::ShopCategory::kTuning
                                             ? shop.selected_slider()
                                             : shop.selected_row();
                  if (cur_sel != size_t(hz.index)) {
                    shop.select_row(size_t(hz.index));
                    sync_shop_preview();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  leave_shop();
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitTab && hz.index >= 0 &&
                        hz.index < int(carview::ShopCategory::kCount)) {
                      shop.set_category(carview::ShopCategory(hz.index));
                      sync_shop_preview();
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                      shop.select_row(size_t(hz.index));
                      fit();
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) shop.move_part(-1);
                else if (e.wheel.y < 0) shop.move_part(1);
                sync_shop_preview();
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The Autoshow takes the keys and buttons while open; a purchase
            // drives the new car away from where this one stands.
            if (autoshow.is_open()) {
              if (app.have_garage) autoshow.set_owned_cars(app.career_garage);
              auto sync_autoshow_camera = [&](bool snap_step_focus) {
                hub_cam.set_framing(autoshow.show_specs() ? 0.11f : 0.0f, 0.16f);
                if (!snap_step_focus) return;
                if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                  // The disc's colour-select camera (CarColorSel), else the engine's.
                  if (const carview::CarCameraPreset* p =
                          hub_preset(carview::hub_camera_preset_name(carview::HubRoom::kAutoshow, -1, false, true)))
                    hub_cam.set_preset(*p);
                  else
                    hub_cam.set_focus(carview::HubCameraFocus::kPaint);
                } else {
                  static constexpr carview::HubCameraFocus kPresets[5] = {
                      carview::HubCameraFocus::kTurntable,
                      carview::HubCameraFocus::kPaint,
                      carview::HubCameraFocus::kFront,
                      carview::HubCameraFocus::kRearThreeQuarter,
                      carview::HubCameraFocus::kSide,
                  };
                  hub_cam.set_focus(kPresets[std::clamp(autoshow.camera_preset(), 0, 4)]);
                }
              };
              auto apply_autoshow_paint = [&]() {
                const carview::AutoshowCar* sel_as = autoshow.selected_car();
                if (!sel_as) return;
                const cars::CarPaintSpec spec = autoshow.preview_paint_spec();
                if (!spec.body.active && !spec.calipers.active) return;
                const std::string& target_media = sel_as->dealer.media_name;
                if (!target_media.empty() && target_media != app.car) {
                  if (auto it = festival_guests.find(target_media); it != festival_guests.end() && it->second) {
                    it->second->set_paint_spec(spec);
                  }
                } else {
                  host.showroom.set_paint_spec(spec);
                }
              };
              auto toggle_specs_or_cam = [&]() {
                autoshow.toggle_specs();
                sync_autoshow_camera(false);
                notice = autoshow.show_specs() ? "SPECS PANEL: SHOWN" : "SPECS PANEL: HIDDEN (CLEAN VIEW)";
                notice_until = vehicle.time() + 2.0;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto cycle_cam = [&]() {
                autoshow.cycle_camera_preset();
                sync_autoshow_camera(true);
                static constexpr const char* kCamNames[5] = {
                    "CAMERA: HERO 3/4", "CAMERA: PAINT CLOSE-UP", "CAMERA: FRONT", "CAMERA: REAR 3/4",
                    "CAMERA: PROFILE"};
                notice = kCamNames[std::clamp(autoshow.camera_preset(), 0, 4)];
                notice_until = vehicle.time() + 2.0;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto test_drive = [&]() {
                std::string td_media;
                int64_t td_cid = 0, td_col = 0;
                if (!autoshow.prepare_test_drive(td_media, td_cid, td_col)) {
                  app.audio_host.trigger_ui(audio::UiCue::kError);
                  return;
                }
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
                autoshow.close();
                app.autoshow_buy_confirm = false;
                app.hub_orbit_dragging = false;
                app.hub_orbit_dragged = false;
                app.pre_loaner_car = app.car;
                app.pre_loaner_garage_id = app.career_garage.selected;
                app.paint_car = td_media;
                app.paint_sequence = td_col;
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
                app.have_spawn = true;
                app.next_car = td_media;
                notice = "TEST DRIVING " + carview::car_name(td_media);
                notice_until = vehicle.time() + 4.0;
                running = false;
              };
              auto buy = [&]() {
                const auto db = app.parts_db.valid() ? app.parts_db.get() : nullptr;
                std::string media, message = "no parts database";
                const bool bought = db && autoshow.buy(*db, app.career_garage, app.career.credits, media, message);
                notice = message;
                notice_until = vehicle.time() + 4;
                if (!bought) {
                  app.autoshow_buy_confirm = false;
                  autoshow.set_buy_confirm_open(false);
                  app.audio_host.trigger_ui(audio::UiCue::kError);
                  return;
                }
                app.audio_host.trigger_ui(audio::UiCue::kPurchase);
                app.audio_host.trigger_ui(audio::UiCue::kCarAdded);
                autoshow.close();
                app.autoshow_buy_confirm = false;
                app.hub_orbit_dragging = false;
                app.hub_orbit_dragged = false;
                app.garage_picker();
                app.write_career_garage();
                ++app.saved.ach.autoshow_purchases;  // achievement 156
                if (!app.career_garage.cars.empty())  // 208 and 215: the car just bought
                  app.note_car_obtained(app.career_garage.cars.back().car_id, true);
                save_now("purchase");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
                app.have_spawn = true;
                app.next_car = media;
                running = false;
              };
              auto confirm_step = [&]() {
                if (autoshow.step() != carview::AutoshowStep::kColorSelect) {
                  app.autoshow_buy_confirm = false;
                  autoshow.set_buy_confirm_open(false);
                  const bool ready_direct = autoshow.advance_step();
                  if (!ready_direct) {
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    sync_autoshow_camera(true);
                    return;
                  }
                }
                if (!autoshow.buy_confirm_open()) {
                  std::string msg;
                  if (!autoshow.request_buy_confirm(app.career.credits, msg)) {
                    app.autoshow_buy_confirm = false;
                    notice = msg;
                    notice_until = vehicle.time() + 4.0;
                    app.audio_host.trigger_ui(audio::UiCue::kError);
                    return;
                  }
                  app.autoshow_buy_confirm = true;
                  notice = msg;
                  notice_until = vehicle.time() + 12.0;
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  return;
                }
                app.autoshow_buy_confirm = false;
                autoshow.set_buy_confirm_open(false);
                buy();
              };
              auto cancel_step = [&]() {
                if (autoshow.buy_confirm_open() || app.autoshow_buy_confirm) {
                  app.autoshow_buy_confirm = false;
                  autoshow.set_buy_confirm_open(false);
                  notice = "PURCHASE CANCELLED";
                  notice_until = vehicle.time() + 2.5;
                  return;
                }
                if (!autoshow.back_step()) {
                  autoshow.close();
                  app.hub_orbit_dragging = false;
                  app.hub_orbit_dragged = false;
                } else {
                  sync_autoshow_camera(true);
                }
              };
              // 115's strip holds the makes alone (the engine's page lists
              // All Affordable and All Cars with them); its Y lists the
              // affordable cars.
              // (make_list_drawn is the last frame's; the step is now's.)
              const bool on_115 = make_list_drawn && autoshow.step() == carview::AutoshowStep::kMakeSelect;
              auto move_mk = [&](int by) {
                if (on_115)
                  autoshow.move_strip_make(by, app.career.credits);
                else
                  autoshow.move_make(by, app.career.credits);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              // Nothing affordable: carflow's NONE AFFORDABLE popup.
              auto affordable = [&]() {
                for (const auto& mk : autoshow.makes()) {
                  if (mk.make_id == -1 && mk.car_count == 0) {
                    make_none_affordable = true;
                    app.audio_host.trigger_ui(audio::UiCue::kError);
                    return;
                  }
                }
                autoshow.select_affordable(app.career.credits);
                confirm_step();
              };
              if (on_115 && make_none_affordable) {
                const bool close =
                    (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                     (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                      e.key.key == SDLK_ESCAPE || e.key.key == SDLK_BACKSPACE)) ||
                    (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                     (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
                      e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) ||
                    (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                if (close) {
                  make_none_affordable = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                        e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
              }
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  cancel_step();
                } else if (autoshow.buy_confirm_open()) {
                  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) confirm_step();
                } else {
                  if (k == SDLK_Y && on_115) {
                    affordable();
                  } else if (k == SDLK_Y) {
                    toggle_specs_or_cam();
                  } else if (k == SDLK_C) {
                    cycle_cam();
                  } else if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                    if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) move_mk(-1);
                    if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) move_mk(1);
                    if (k == SDLK_Q || k == SDLK_PAGEUP) move_mk(-6);
                    if (k == SDLK_E || k == SDLK_PAGEDOWN) move_mk(6);
                  } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                    if (k == SDLK_LEFT || k == SDLK_A) { app.autoshow_buy_confirm = false; autoshow.move_colour(-1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_RIGHT || k == SDLK_D) { app.autoshow_buy_confirm = false; autoshow.move_colour(1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_UP || k == SDLK_W) { autoshow.cycle_caliper_option(-1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_DOWN || k == SDLK_S) { autoshow.cycle_caliper_option(1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_Q) { autoshow.cycle_paint_finish(-1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_E) { autoshow.cycle_paint_finish(1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_T || k == SDLK_X) { test_drive(); }
                  } else {
                    if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) { autoshow.move(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) { autoshow.move(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_Q || k == SDLK_PAGEUP) { autoshow.page_move_car(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_E || k == SDLK_PAGEDOWN) { autoshow.page_move_car(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_TAB || k == SDLK_M) { autoshow.toggle_all_cars(app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_X || k == SDLK_R) { autoshow.cycle_sort(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_G) { autoshow.cycle_class_filter(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_V) { autoshow.cycle_drivetrain_filter(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (k == SDLK_T) { test_drive(); }
                  }
                  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { confirm_step(); }
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  cancel_step();
                } else if (autoshow.buy_confirm_open()) {
                  if (b == SDL_GAMEPAD_BUTTON_SOUTH) confirm_step();
                } else {
                  if (b == SDL_GAMEPAD_BUTTON_NORTH && on_115) {
                    affordable();
                  } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                    toggle_specs_or_cam();
                  } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
                    cycle_cam();
                  } else if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) move_mk(-1);
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) move_mk(1);
                    if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) move_mk(-6);
                    if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) move_mk(6);
                  } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) { app.autoshow_buy_confirm = false; autoshow.move_colour(-1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) { app.autoshow_buy_confirm = false; autoshow.move_colour(1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) { autoshow.cycle_caliper_option(-1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { autoshow.cycle_caliper_option(1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { autoshow.cycle_paint_finish(-1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { autoshow.cycle_paint_finish(1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_WEST) { test_drive(); }
                  } else {
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) { autoshow.move(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { autoshow.move(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { autoshow.page_move_car(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { autoshow.page_move_car(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_BACK) { autoshow.toggle_all_cars(app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_WEST) { autoshow.cycle_sort(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    if (b == SDL_GAMEPAD_BUTTON_LEFT_STICK) { autoshow.cycle_class_filter(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  }
                  if (b == SDL_GAMEPAD_BUTTON_SOUTH) { confirm_step(); }
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                handle_hub_orbit_input(app, hub_cam, e, false, true);
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                if (handle_hub_orbit_input(app, hub_cam, e, false, false)) {
                  return true;
                }
                if (!autoshow.buy_confirm_open()) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                    if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                      if (autoshow.selected_make_index() != size_t(hz.index)) {
                        autoshow.select_make(size_t(hz.index), app.career.credits);
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      }
                    } else if (autoshow.step() == carview::AutoshowStep::kCarSelect) {
                      if (autoshow.selected_index() != size_t(hz.index)) {
                        autoshow.select_index(size_t(hz.index));
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  cancel_step();
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                  handle_hub_orbit_input(app, hub_cam, e, false, false);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  const bool hit_ui = ui_mouse_hit(e.button.x, e.button.y, hz);
                  if (hit_ui && hz.group == carview::ui::kHitConfirm) {
                    confirm_step();
                  } else if (hit_ui && hz.group == carview::ui::kHitCancel) {
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                    cancel_step();
                  } else if (hit_ui && hz.group == carview::ui4::kHitButton) {
                    if (autoshow.buy_confirm_open()) {
                      if (hz.index == 0) confirm_step();
                      else if (hz.index == 1) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                    } else if (on_115) {
                      // 115's help bar: A SELECT, B BACK, Y ALL AFFORDABLE CARS.
                      if (hz.index == 0) confirm_step();
                      else if (hz.index == 1) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                      else if (hz.index == 2) affordable();
                    } else if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                      if (hz.index == 0) confirm_step();
                      else if (hz.index == 1) { autoshow.page_move_make(1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                      else if (hz.index == 2) toggle_specs_or_cam();
                      else if (hz.index == 3) cycle_cam();
                      else if (hz.index == 4) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                    } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                      if (hz.index == 0) confirm_step();
                      else if (hz.index == 1) test_drive();
                      else if (hz.index == 2) { autoshow.cycle_paint_finish(1); apply_autoshow_paint(); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                      else if (hz.index == 3) toggle_specs_or_cam();
                      else if (hz.index == 4) cycle_cam();
                      else if (hz.index == 5) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                    } else if (car_list_drawn) {
                      // 108's help bar: A SELECT, B BACK, Y TOGGLE, SEARCH.
                      if (hz.index == 0) confirm_step();
                      else if (hz.index == 1) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                      else if (hz.index == 2) toggle_specs_or_cam();
                      else if (hz.index == 3) { autoshow.toggle_all_cars(app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                    } else {
                      if (hz.index == 0) confirm_step();
                      else if (hz.index == 1) { autoshow.cycle_sort(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                      else if (hz.index == 2) toggle_specs_or_cam();
                      else if (hz.index == 3) { autoshow.page_move_car(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                      else if (hz.index == 4) cycle_cam();
                      else if (hz.index == 5) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                    }
                  } else if (!autoshow.buy_confirm_open() && hit_ui && hz.group == carview::ui::kHitTab) {
                    if (hz.index == 0) autoshow.cycle_sort(1);
                    else if (hz.index == 1) autoshow.cycle_class_filter(1);
                    else if (hz.index == 2) autoshow.cycle_drivetrain_filter(1);
                    else if (hz.index == 3) autoshow.toggle_all_cars(app.career.credits);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (!autoshow.buy_confirm_open() && hit_ui && hz.group == carview::ui4::kHitRegion) {
                    if (hz.index == 0) autoshow.cycle_paint_finish(1);
                    else if (hz.index == 1) autoshow.cycle_caliper_option(1);
                    apply_autoshow_paint();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (!autoshow.buy_confirm_open() && hit_ui && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                    if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                      autoshow.select_make(size_t(hz.index), app.career.credits);
                      confirm_step();
                    } else if (autoshow.step() == carview::AutoshowStep::kCarSelect) {
                      autoshow.select_index(size_t(hz.index));
                      confirm_step();
                    }
                  } else if (!autoshow.buy_confirm_open() && autoshow.step() == carview::AutoshowStep::kColorSelect) {
                    int sw_idx = (hit_ui && hz.group == carview::ui4::kHitSwatch && hz.index >= 0)
                                     ? hz.index
                                     : autoshow_swatch_hit_index(app, autoshow, last_ui_w, last_ui_h, e.button.x,
                                                                 e.button.y);
                    if (sw_idx >= 0) {
                      // Clicking a color swatch previews that color only — never triggers immediate purchase!
                      const int delta = sw_idx - int(autoshow.selected_colour_index());
                      if (delta != 0) autoshow.move_colour(delta);
                      apply_autoshow_paint();
                      app.autoshow_buy_confirm = false;
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else {
                      handle_hub_orbit_input(app, hub_cam, e, false, false);
                    }
                  } else if (!hit_ui) {
                    handle_hub_orbit_input(app, hub_cam, e, false, false);
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                handle_hub_orbit_input(app, hub_cam, e, false, false);
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL && !autoshow.buy_confirm_open()) {
                const int dir = e.wheel.y > 0 ? -1 : 1;
                if (autoshow.step() == carview::AutoshowStep::kMakeSelect) { move_mk(dir); return true; }
                else if (autoshow.step() == carview::AutoshowStep::kColorSelect) { app.autoshow_buy_confirm = false; autoshow.move_colour(dir); apply_autoshow_paint(); }
                else autoshow.move(dir);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // Horizon Rally Hub / Service Area / Championship Results input.
            if (app.rally_results_open) {
              const bool confirm = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                                    (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                                     e.key.key == SDLK_ESCAPE)) ||
                                   (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                    (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                                     e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) ||
                                   (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                                    (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT));
              if (confirm) {
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
                app.rally_results_open = false;
                // The intro race's end (game_rally_intro_flow): to the first
                // Autoshow, then the hub's first visit.
                const bool intro_heats = app.rally_session.is_heats() && app.saved.rally_flow == 1;
                app.rally_session.retire();
                if (intro_heats) {
                  app.audio_host.trigger_vo("PostraceHeatsQualification", false);
                  app.audio_host.trigger_vo("ToTheAutoshow", false);
                  app.saved.rally_flow = 2;
                  app.rally_first_autoshow_pending = true;
                  app.rally_hub_open = false;
                } else {
                  app.rally_hub_open = true;
                }
                app.leave_race = true;
                running = false;
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (app.rally_service_open) {
              // Between stages (game_rally_race_flow): the stage's results,
              // then the next stage. The race flow repairs every car before
              // each stage (reset_car_damage_all_cars); there is no service
              // menu, and quitting is the pre-race menu's or the pause menu's.
              auto act_service = [&]() {
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
                car_damage.reset();
                app.rally_service_repaired = true;
                app.rally_session.advance_from_service_area();
                app.rally_service_open = false;
                app.rally_service_return = false;
                app.rally_notice.clear();
                if (const auto* stg = app.rally_session.current_stage()) {
                  app.race_options.stage_title = stg->name;
                  app.next_event = "stage:" + std::to_string(stg->route_id);
                  running = false;
                }
              };
              app.rally_service_selected = 0;
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE) act_service();
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) act_service();
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui4::kHitButton && hz.index == 0)
                  act_service();
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
  return false;
}

bool DriveSession::handle_rally_career_club_event(const SDL_Event& e) {
            // The Rally hub (H_RALLY_HUB): left/right the tickets, A enters, Y the
            // track layout, B the QUIT HORIZON RALLY popup, BACK the engine's
            // EXTRAS (Q211). The popup takes the input while it is up.
            if (app.rally_hub_open && app.rally_catalog.loaded && rally_entry == carview::RallyEntryPopup::kNone) {
              const bool consumed = (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                                     e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
              // first_time_hub_flow: no input while the help bar is off.
              if (app.rally_hub_tutorial != 0) return consumed;
              int64_t cid = 0;
              int clid = 4;
              current_car_info(cid, clid);
              const int disc = host.free_roam ? freeroam::upgrade_discount(host.free_roam->signs_found()) : 0;
              const auto db_ptr = app.parts_db.valid() ? app.parts_db.get() : std::shared_ptr<cars::PartsDb>{};
              const auto hst = carview::make_rally_hub_state(app.rally_catalog, app.saved, app.have_garage ? &app.career_garage : nullptr,
                                                             db_ptr.get(), app.class_bands, app.car, clid, 0, disc,
                                                             app.rally_hub_selected, app.rally_notice);
              const int tickets = hst.tickets;
              const int extras = int(hst.items.size()) - tickets;
              auto activate_item = [&](const carview::RallyHubItem& it) {
                if (it.kind == carview::RallyHubItem::kHeats) {
                  if (const std::string id = app.start_rally_heats(); !id.empty()) {
                    app.rally_hub_extras = false;
                    app.next_event = id;
                    running = false;
                  }
                } else if (it.kind == carview::RallyHubItem::kChampionship) {
                  if (!it.eligible) {
                    app.rally_notice = it.lock_reason;
                    return;
                  }
                  if (app.rally_session.start_championship(app.rally_catalog, it.event_id, int(app.race_options.difficulty),
                                                           app.race_options.seed)) {
                    if (const auto* stg = app.rally_session.current_stage()) {
                      app.rally_hub_open = false;
                      app.rally_hub_track_layout = false;
                      app.race_options.stage_title = stg->name;
                      app.next_event = "stage:" + std::to_string(stg->route_id);
                      running = false;
                    }
                  }
                } else if (it.kind == carview::RallyHubItem::kFitRallyKit) {
                  if (app.have_garage && db_ptr && db_ptr->is_open()) {
                    if (const garage::Car* sc = app.career_garage.selected_car()) {
                      const auto plan = garage::plan_rally_kit(app.career_garage, *db_ptr, sc->id, disc, true,
                                                               &hub_phys_settings, &hub_pi_cfg,
                                                               !app.class_bands.empty() ? &app.class_bands : nullptr);
                      std::string msg;
                      if (garage::apply_rally_kit(app.career_garage, *db_ptr, sc->id, plan, disc, app.career.credits,
                                                  &hub_phys_settings, &hub_pi_cfg,
                                                  !app.class_bands.empty() ? &app.class_bands : nullptr, &msg)) {
                        app.saved.credits = app.career.credits;
                        app.write_career_garage();
                        save_now("rally kit");
                        app.rally_notice = msg;
                        hot_reload_car = true;
                        running = false;
                      } else {
                        app.rally_notice = msg;
                      }
                    }
                  }
                } else if (it.kind == carview::RallyHubItem::kFreeRoam) {
                  app.rally_hub_extras = false;
                  app.rally_hub_open = false;
                } else if (it.kind == carview::RallyHubItem::kReturnColorado) {
                  app.rally_hub_extras = false;
                  app.rally_hub_open = false;
                  app.rally_world_switch = -1;
                  running = false;
                }
              };
              enum class Press { kNone, kAccept, kBack, kPrev, kNext, kY, kExtras };
              Press p = Press::kNone;
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_UP || k == SDLK_W || k == SDLK_LEFT || k == SDLK_A) p = Press::kPrev;
                else if (k == SDLK_DOWN || k == SDLK_S || k == SDLK_RIGHT || k == SDLK_D) p = Press::kNext;
                else if (!e.key.repeat) {
                  if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) p = Press::kBack;
                  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) p = Press::kAccept;
                  else if (k == SDLK_Y) p = Press::kY;
                  else if (k == SDLK_M || k == SDLK_TAB) p = Press::kExtras;
                }
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST) p = Press::kBack;
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) p = Press::kAccept;
                else if (b == SDL_GAMEPAD_BUTTON_NORTH) p = Press::kY;
                else if (b == SDL_GAMEPAD_BUTTON_BACK) p = Press::kExtras;
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) p = Press::kPrev;
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) p = Press::kNext;
              } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                p = e.wheel.y > 0 ? Press::kPrev : e.wheel.y < 0 ? Press::kNext : Press::kNone;
              } else if (e.type == SDL_EVENT_MOUSE_MOTION && app.rally_hub_extras) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= tickets &&
                    hz.index - tickets < extras && app.rally_hub_extra_selected != hz.index - tickets) {
                  app.rally_hub_extra_selected = hz.index - tickets;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) p = Press::kBack;
                else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && app.rally_hub_extras && hz.index >= tickets &&
                        hz.index - tickets < extras) {
                      app.rally_hub_extra_selected = hz.index - tickets;
                      p = Press::kAccept;
                    } else if (hz.group == carview::ui4::kHitButton) {
                      static constexpr Press kBar[] = {Press::kAccept, Press::kBack, Press::kY, Press::kExtras};
                      if (hz.index >= 0 && hz.index < 4) p = kBar[hz.index];
                    }
                  }
                }
              }
              if (app.rally_hub_extras) {
                const int n = std::max(1, extras);
                if (p == Press::kPrev) app.rally_hub_extra_selected = (app.rally_hub_extra_selected + n - 1) % n;
                else if (p == Press::kNext) app.rally_hub_extra_selected = (app.rally_hub_extra_selected + 1) % n;
                else if (p == Press::kBack || p == Press::kExtras) app.rally_hub_extras = false;
                else if (p == Press::kAccept && extras > 0) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  activate_item(hst.items[size_t(tickets + std::clamp(app.rally_hub_extra_selected, 0, extras - 1))]);
                }
                if (p == Press::kPrev || p == Press::kNext) app.audio_host.trigger_ui(audio::UiCue::kMove);
                if (p == Press::kBack || p == Press::kExtras) app.audio_host.trigger_ui(audio::UiCue::kCancel);
                return consumed;
              }
              switch (p) {
                case Press::kPrev:
                case Press::kNext: {
                  const int to = std::clamp(app.rally_hub_selected + (p == Press::kNext ? 1 : -1), 0, std::max(0, tickets - 1));
                  if (to != app.rally_hub_selected) {
                    app.rally_hub_selected = to;
                    app.rally_hub_track_layout = false;
                    app.rally_notice.clear();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                  break;
                }
                case Press::kAccept:
                  if (tickets > 0) {
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    activate_item(hst.items[size_t(std::clamp(app.rally_hub_selected, 0, tickets - 1))]);
                  }
                  break;
                case Press::kY:
                  if (tickets > 0) {
                    app.rally_hub_track_layout = !app.rally_hub_track_layout;
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                  break;
                case Press::kBack:
                  if (app.rally_hub_track_layout) {
                    app.rally_hub_track_layout = false;
                  } else {
                    // rally_event_entry_flow's return: the QUIT HORIZON RALLY popup.
                    rally_entry = carview::RallyEntryPopup::kQuit;
                    app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
                    break;
                  }
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  break;
                case Press::kExtras:
                  app.rally_hub_extras = true;
                  app.rally_hub_extra_selected = 0;
                  app.rally_notice.clear();
                  app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
                  break;
                case Press::kNone: break;
              }
              return consumed;
            }
            // Horizon Rally's entry popups (A OK, B CANCEL).
            if (rally_entry != carview::RallyEntryPopup::kNone) {
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) rally_entry_press(false);
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) rally_entry_press(true);
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_BACK) rally_entry_press(false);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) rally_entry_press(true);
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) rally_entry_press(false);
                else if (e.button.button == SDL_BUTTON_LEFT) rally_entry_press(true);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The 1000 Club's menus on their scenes (club_menu.h).
            if (club_open && club_menu.open()) {
              using CB = carview::ClubMenu::Button;
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) club_menu_press(CB::kBack);
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) club_menu_press(CB::kAccept);
                else if (k == SDLK_UP || k == SDLK_W) club_menu_press(CB::kUp);
                else if (k == SDLK_DOWN || k == SDLK_S) club_menu_press(CB::kDown);
                else if (k == SDLK_LEFT || k == SDLK_A) club_menu_press(CB::kLeft);
                else if (k == SDLK_RIGHT || k == SDLK_D) club_menu_press(CB::kRight);
              } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_UP || k == SDLK_W) club_menu_press(CB::kUp);
                else if (k == SDLK_DOWN || k == SDLK_S) club_menu_press(CB::kDown);
                else if (k == SDLK_LEFT || k == SDLK_A) club_menu_press(CB::kLeft);
                else if (k == SDLK_RIGHT || k == SDLK_D) club_menu_press(CB::kRight);
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_BACK) club_menu_press(CB::kBack);
                else if (b == SDL_GAMEPAD_BUTTON_START) {
                  club_menu.close();
                  club_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) club_menu_press(CB::kAccept);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) club_menu_press(CB::kUp);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) club_menu_press(CB::kDown);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) club_menu_press(CB::kLeft);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) club_menu_press(CB::kRight);
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) club_menu_press(CB::kBack);
                else if (e.button.button == SDL_BUTTON_LEFT) club_menu_press(CB::kAccept);
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) club_menu_press(CB::kUp);
                else if (e.wheel.y < 0) club_menu_press(CB::kDown);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The hand-drawn 1000 Club screen (no v4 scenes): up/down inspects challenges & hints, enter begins challenges in this car.
            if (club_open) {
              const size_t rows = (club_gate.empty() && club_car >= 0 && app.club_data.cars.count(club_car))
                                      ? std::max<size_t>(1, app.club_data.cars.at(club_car).size())
                                      : 1;
              auto begin = [&]() {
                if (!club_gate.empty() || club_car < 0 || !app.club_data.cars.count(club_car) || club_record()) return;
                drive::SavedClubCar rec;
                rec.car_id = club_car;
                for (const auto& ch : app.club_data.cars.at(club_car)) {
                  rec.challenges.emplace_back(ch.definition_hash, false);
                  rec.progress.emplace_back();
                }
                app.saved.club.push_back(std::move(rec));
                save_now("1000 club");
                club_check();
                club_open = false;
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) { club_open = false; app.audio_host.trigger_ui(audio::UiCue::kCancel); }
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) { app.audio_host.trigger_ui(audio::UiCue::kAccept); begin(); }
                else if (k == SDLK_UP || k == SDLK_W) { club_pick = (club_pick + rows - 1) % rows; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (k == SDLK_DOWN || k == SDLK_S) { club_pick = (club_pick + 1) % rows; app.audio_host.trigger_ui(audio::UiCue::kMove); }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) { club_open = false; app.audio_host.trigger_ui(audio::UiCue::kCancel); }
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) { app.audio_host.trigger_ui(audio::UiCue::kAccept); begin(); }
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) { club_pick = (club_pick + rows - 1) % rows; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { club_pick = (club_pick + 1) % rows; app.audio_host.trigger_ui(audio::UiCue::kMove); }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow &&
                    hz.index >= 0 && size_t(hz.index) < rows && club_pick != size_t(hz.index)) {
                  club_pick = size_t(hz.index);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  club_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && hz.index >= 0 && size_t(hz.index) < rows) {
                      club_pick = size_t(hz.index);
                      if (!club_record()) {
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        begin();
                      } else {
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      }
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (!club_gate.empty() || hz.index == 2) {
                        club_open = false;
                        app.audio_host.trigger_ui(audio::UiCue::kCancel);
                      } else if (hz.index == 0) {
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        begin();
                      }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) club_pick = (club_pick + rows - 1) % rows;
                else if (e.wheel.y < 0) club_pick = (club_pick + 1) % rows;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // Offline Car Club & Shared Club Garage (451_CLUB_SETTINGS).
            if (car_club_open) {
              static const std::vector<std::string> kClubNames = {
                  "HORIZON DRIVERS", "COLORADO APEX", "RED ROCK RACERS", "PIKES PEAK CREW", "FESTIVAL LEGENDS",
                  "SHIFT COLLECTIVE"};
              static const std::vector<std::string> kClubTags = {"HRZN", "APEX", "ROCK", "PEAK", "FEST", "SHFT"};
              const int n_cars = int(app.baked_cars.size());
              const int total_rows = 2 + n_cars;
              auto& cc_ed = carview::car_club_editor_state();
              auto sync_club_editor = [&]() {
                cc_ed.has_garage_sync = app.have_garage;
                cc_ed.row_garage_ids.clear();
                cc_ed.row_garage_ids.reserve(size_t(std::max(0, n_cars)));
                for (int ci = 0; ci < n_cars; ++ci) {
                  int64_t gid = 0;
                  if (app.have_garage && size_t(ci) < app.pick_ids.size()) gid = app.pick_ids[size_t(ci)];
                  cc_ed.row_garage_ids.push_back(gid);
                }
                if (app.have_garage) {
                  cc_ed.shared_garage_ids = app.career_garage.club_shared_garage_ids;
                  cc_ed.borrowed_garage_id = app.career_garage.club_borrowed_garage_id;
                } else {
                  cc_ed.shared_garage_ids.clear();
                  cc_ed.borrowed_garage_id = 0;
                }
              };
              sync_club_editor();

              // Modal On-Screen Keyboard for custom Club Name & 4-char Tag entry.
              if (cc_ed.osk.is_open()) {
                auto commit_osk = [&]() {
                  if (cc_ed.osk.confirmed()) {
                    if (cc_ed.osk_target == carview::CarClubEditorState::OskTarget::kClubName) {
                      const std::string clean = carview::sanitize_club_name(cc_ed.osk.text(), 24);
                      if (!clean.empty()) {
                        app.saved.car_club_name = clean;
                        car_club_notice = "CLUB NAME: " + app.saved.car_club_name;
                        save_now("car club");
                      }
                    } else if (cc_ed.osk_target == carview::CarClubEditorState::OskTarget::kClubTag) {
                      app.saved.car_club_tag = carview::sanitize_club_tag(cc_ed.osk.text());
                      car_club_notice = "CLUB TAG: [" + app.saved.car_club_tag + "]";
                      save_now("car club");
                    }
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  } else {
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  }
                  cc_ed.osk_target = carview::CarClubEditorState::OskTarget::kNone;
                  if (app.window) SDL_StopTextInput(app.window);
                };
                if (e.type == SDL_EVENT_TEXT_INPUT && e.text.text) {
                  for (const char* p = e.text.text; *p; ++p) {
                    if (static_cast<unsigned char>(*p) >= 32 && static_cast<unsigned char>(*p) <= 126) {
                      cc_ed.osk.type_char(*p);
                    }
                  }
                } else if (e.type == SDL_EVENT_KEY_DOWN) {
                  const SDL_Keycode k = e.key.key;
                  if (k == SDLK_ESCAPE && !e.key.repeat) {
                    cc_ed.osk.cancel();
                    commit_osk();
                  } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && !e.key.repeat) {
                    cc_ed.osk.confirm();
                    commit_osk();
                  } else if (k == SDLK_BACKSPACE) {
                    cc_ed.osk.backspace();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_LEFT) {
                    cc_ed.osk.move(-1, 0);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_RIGHT) {
                    cc_ed.osk.move(1, 0);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_UP) {
                    cc_ed.osk.move(0, -1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_DOWN) {
                    cc_ed.osk.move(0, 1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                  const auto b = e.gbutton.button;
                  if (b == SDL_GAMEPAD_BUTTON_EAST) {
                    cc_ed.osk.cancel();
                    commit_osk();
                  } else if (b == SDL_GAMEPAD_BUTTON_START) {
                    cc_ed.osk.confirm();
                    commit_osk();
                  } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                    cc_ed.osk.move(-1, 0);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                    cc_ed.osk.move(1, 0);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                    cc_ed.osk.move(0, -1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                    cc_ed.osk.move(0, 1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                    cc_ed.osk.press_selected();
                    if (!cc_ed.osk.is_open()) commit_osk();
                    else app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                    cc_ed.osk.backspace();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                    cc_ed.osk.type_char(' ');
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                    cc_ed.osk.toggle_shift();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                    cc_ed.osk.toggle_symbols();
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                  int tr = -1, tc = -1;
                  if (hit_test_osk_key_hub(app.window, last_ui_w, last_ui_h, e.motion.x,
                                           e.motion.y, cc_ed.osk.symbols(), tr, tc)) {
                    if (tr != cc_ed.osk.cursor_row() || tc != cc_ed.osk.cursor_col()) {
                      for (int s_i = 0; s_i < 12 && cc_ed.osk.cursor_row() != tr; ++s_i)
                        cc_ed.osk.move(0, tr > cc_ed.osk.cursor_row() ? 1 : -1);
                      for (int s_i = 0; s_i < 12 && cc_ed.osk.cursor_col() != tc; ++s_i)
                        cc_ed.osk.move(tc > cc_ed.osk.cursor_col() ? 1 : -1, 0);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    }
                  }
                } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                  if (e.button.button == SDL_BUTTON_RIGHT) {
                    cc_ed.osk.cancel();
                    commit_osk();
                  } else if (e.button.button == SDL_BUTTON_LEFT) {
                    int tr = -1, tc = -1;
                    carview::ui::HitZone hz{};
                    if (hit_test_osk_key_hub(app.window, last_ui_w, last_ui_h, e.button.x,
                                             e.button.y, cc_ed.osk.symbols(), tr, tc)) {
                      for (int s_i = 0; s_i < 12 && cc_ed.osk.cursor_row() != tr; ++s_i)
                        cc_ed.osk.move(0, tr > cc_ed.osk.cursor_row() ? 1 : -1);
                      for (int s_i = 0; s_i < 12 && cc_ed.osk.cursor_col() != tc; ++s_i)
                        cc_ed.osk.move(tc > cc_ed.osk.cursor_col() ? 1 : -1, 0);
                      cc_ed.osk.press_selected();
                      if (!cc_ed.osk.is_open()) commit_osk();
                      else app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) {
                        cc_ed.osk.press_selected();
                        if (!cc_ed.osk.is_open()) commit_osk();
                        else app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 1) {
                        cc_ed.osk.backspace();
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 2) {
                        cc_ed.osk.confirm();
                        commit_osk();
                      } else if (hz.index == 3) {
                        cc_ed.osk.cancel();
                        commit_osk();
                      }
                    }
                  }
                }
                return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                        e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
              }

              auto open_club_osk = [&](bool for_tag) {
                if (for_tag) {
                  cc_ed.osk_target = carview::CarClubEditorState::OskTarget::kClubTag;
                  cc_ed.osk.open("EDIT CLUB TAG (4 CHARS)", app.saved.car_club_tag, 4,
                                 carview::OnScreenKeyboard::Purpose::kLicensePlate);
                } else {
                  cc_ed.osk_target = carview::CarClubEditorState::OskTarget::kClubName;
                  cc_ed.osk.open("EDIT CAR CLUB NAME", app.saved.car_club_name, 24,
                                 carview::OnScreenKeyboard::Purpose::kGamertag);
                }
                if (app.window) SDL_StartTextInput(app.window);
                app.audio_host.trigger_ui(audio::UiCue::kAccept);
              };
              auto cycle_name = [&](int dir) {
                int idx = 0;
                for (int i = 0; i < int(kClubNames.size()); ++i)
                  if (kClubNames[size_t(i)] == app.saved.car_club_name) idx = i;
                idx = ((idx + dir) % int(kClubNames.size()) + int(kClubNames.size())) % int(kClubNames.size());
                app.saved.car_club_name = kClubNames[size_t(idx)];
                car_club_notice = "CLUB NAME: " + app.saved.car_club_name;
                save_now("car club");
              };
              auto cycle_tag = [&](int dir) {
                int idx = 0;
                for (int i = 0; i < int(kClubTags.size()); ++i)
                  if (kClubTags[size_t(i)] == app.saved.car_club_tag) idx = i;
                idx = ((idx + dir) % int(kClubTags.size()) + int(kClubTags.size())) % int(kClubTags.size());
                app.saved.car_club_tag = carview::sanitize_club_tag(kClubTags[size_t(idx)]);
                car_club_notice = "CLUB TAG: [" + app.saved.car_club_tag + "]";
                save_now("car club");
              };
              auto garage_id_at = [&](int ci) -> int64_t {
                if (ci < 0 || ci >= n_cars) return 0;
                if (app.have_garage && size_t(ci) < app.pick_ids.size()) return app.pick_ids[size_t(ci)];
                return 0;
              };
              auto car_id_at = [&](int ci) -> int {
                if (ci < 0 || ci >= n_cars) return 0;
                if (app.have_garage && size_t(ci) < app.pick_ids.size()) {
                  if (const garage::Car* gc = app.career_garage.find(app.pick_ids[size_t(ci)])) return int(gc->car_id);
                }
                if (app.have_career_catalog) {
                  if (const auto* sp = app.career_catalog.find_car_by_media(app.baked_cars[size_t(ci)]))
                    return int(sp->car_id);
                }
                return ci + 1;
              };
              auto activate_row = [&](int dir) {
                if (car_club_pick == 0) {
                  cycle_name(dir);
                } else if (car_club_pick == 1) {
                  cycle_tag(dir);
                } else {
                  const int ci = car_club_pick - 2;
                  const int64_t gid = garage_id_at(ci);
                  if (app.have_garage && gid > 0) {
                    const bool now_shared = app.career_garage.toggle_club_shared(gid);
                    // Mirror instance IDs into profile club list for legacy summary display.
                    app.saved.car_club_shared.clear();
                    for (int64_t sgid : app.career_garage.club_shared_garage_ids) {
                      app.saved.car_club_shared.push_back(int(sgid));
                    }
                    sync_club_editor();
                    car_club_notice = (now_shared ? "SHARED TO CLUB (#" + std::to_string(gid) + "): "
                                                  : "REMOVED FROM CLUB (#" + std::to_string(gid) + "): ") +
                                      carview::car_name(app.baked_cars[size_t(ci)]);
                    app.write_career_garage();
                    save_now("car club");
                  } else {
                    const int cid = car_id_at(ci);
                    if (cid > 0) {
                      const bool now_shared = carview::toggle_club_shared_car(app.saved.car_club_shared, cid);
                      car_club_notice = (now_shared ? "SHARED TO CLUB: " : "REMOVED FROM CLUB: ") +
                                        carview::car_name(app.baked_cars[size_t(ci)]);
                      save_now("car club");
                    }
                  }
                }
              };
              auto drive_shared_selected = [&]() {
                if (car_club_pick < 2 || car_club_pick >= total_rows) return;
                const int ci = car_club_pick - 2;
                const int64_t gid = garage_id_at(ci);
                if (app.have_garage && gid > 0) {
                  if (app.career_garage.club_borrowed_garage_id == gid) {
                    // Return borrowed car to the player's prior garage car.
                    app.career_garage.return_borrowed_club_car();
                    sync_club_editor();
                    app.write_career_garage();
                    save_now("car club");
                    car_club_open = false;
                    if (const garage::Car* ret = app.career_garage.selected_car()) {
                      if (auto bit = app.baked_ids.find(ret->car_id);
                          bit != app.baked_ids.end() && bit->second != app.car) {
                        app.next_car = bit->second;
                        running = false;
                        return;
                      }
                    }
                    notice = "RETURNED BORROWED CLUB CAR";
                    notice_until = vehicle.time() + 2.5;
                    return;
                  }
                  if (!app.career_garage.is_club_shared(gid)) {
                    car_club_notice = "SHARE THIS GARAGE CAR FIRST TO BORROW IT";
                    return;
                  }
                  app.career_garage.borrow_club_car(gid);
                  sync_club_editor();
                  app.write_career_garage();
                  save_now("car club");
                  car_club_open = false;
                  if (app.baked_cars[size_t(ci)] != app.car) {
                    app.next_car = app.baked_cars[size_t(ci)];
                    running = false;
                  } else {
                    notice = "BORROWED CLUB CAR (#" + std::to_string(gid) + "): " + carview::car_name(app.car);
                    notice_until = vehicle.time() + 2.5;
                  }
                  return;
                }
                const int cid = car_id_at(ci);
                const bool is_shared = std::find(app.saved.car_club_shared.begin(), app.saved.car_club_shared.end(),
                                                 cid) != app.saved.car_club_shared.end();
                if (!is_shared) {
                  car_club_notice = "SHARE THIS CAR FIRST TO BORROW IT";
                  return;
                }
                car_club_open = false;
                if (app.baked_cars[size_t(ci)] != app.car) {
                  app.next_car = app.baked_cars[size_t(ci)];
                  running = false;
                } else {
                  notice = "DRIVING CLUB CAR: " + carview::car_name(app.car);
                  notice_until = vehicle.time() + 2.5;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) {
                  car_club_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_UP || k == SDLK_W) {
                  car_club_pick = ((car_club_pick - 1) % total_rows + total_rows) % total_rows;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_DOWN || k == SDLK_S) {
                  car_club_pick = (car_club_pick + 1) % total_rows;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_LEFT || k == SDLK_A) {
                  if (car_club_pick < 2) { activate_row(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                } else if (k == SDLK_RIGHT) {
                  if (car_club_pick < 2) { activate_row(+1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                } else if ((k == SDLK_Y || k == SDLK_E || k == SDLK_F2) && !e.key.repeat) {
                  open_club_osk(car_club_pick == 1);
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) {
                  activate_row(+1);
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if ((k == SDLK_D || k == SDLK_X) && !e.key.repeat) {
                  drive_shared_selected();
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) {
                  car_club_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  car_club_pick = ((car_club_pick - 1) % total_rows + total_rows) % total_rows;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  car_club_pick = (car_club_pick + 1) % total_rows;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT && car_club_pick < 2) {
                  activate_row(-1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT && car_club_pick < 2) {
                  activate_row(+1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                  open_club_osk(car_club_pick == 1);
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  activate_row(+1);
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  drive_shared_selected();
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow &&
                    hz.index >= 0 && hz.index < total_rows && car_club_pick != hz.index) {
                  car_club_pick = hz.index;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  car_club_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && hz.index >= 0 && hz.index < total_rows) {
                      car_club_pick = hz.index;
                      if (hz.index < 2) open_club_osk(hz.index == 1);
                      else { activate_row(+1); app.audio_host.trigger_ui(audio::UiCue::kAccept); }
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) { activate_row(+1); app.audio_host.trigger_ui(audio::UiCue::kAccept); }
                      else if (hz.index == 1) { open_club_osk(car_club_pick == 1); }
                      else if (hz.index == 2) { drive_shared_selected(); app.audio_host.trigger_ui(audio::UiCue::kAccept); }
                      else if (hz.index == 3) { car_club_open = false; app.audio_host.trigger_ui(audio::UiCue::kCancel); }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL && total_rows > 0) {
                car_club_pick = ((car_club_pick + (e.wheel.y > 0 ? -1 : 1)) % total_rows + total_rows) % total_rows;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // Paint Shop & Vinyl/Livery Editor (LiveryEditor live preview + .liv persistence; legacy swatch fallback).
            if (paint_open && livery_editor.is_open()) {
              auto liveries_dir = [&]() -> std::filesystem::path {
                if (!livery_edit_path.empty() && livery_edit_path.has_parent_path()) return livery_edit_path.parent_path();
                if (!app.save_path.empty()) return app.save_path.parent_path() / "liveries";
                return std::filesystem::path("liveries");
              };
              auto close_livery = [&](bool save_changes) {
                if (save_changes) {
                  std::string lerr;
                  const bool ok = livery_editor.save(livery_edit_path, &lerr);
                  sync_livery_preview(!livery_editor.document().layers.empty());
                  livery_editor.mark_vinyl_preview_uploaded(double(SDL_GetTicksNS()) * 1e-9);
                  if (ok && app.have_garage) {
                    if (const garage::Car* sel = app.career_garage.selected_car()) {
                      if (garage::Car* gc = app.career_garage.find(sel->id);
                          gc && app.baked_ids.count(gc->car_id) && lower_ascii(app.baked_ids.at(gc->car_id)) == lower_ascii(app.car)) {
                        std::error_code ec;
                        const auto rel = !app.save_path.empty()
                                             ? std::filesystem::relative(livery_edit_path, app.save_path.parent_path(), ec)
                                             : std::filesystem::path{};
                        gc->livery_file = (!ec && !rel.empty()) ? rel.string() : livery_edit_path.string();
                        app.write_career_garage();
                      }
                    }
                  }
                  if (ok) ++app.saved.ach.paint_jobs_saved;  // achievement 167
                  save_now("livery");
                  notice = ok ? "LIVERY SAVED & APPLIED" : ("LIVERY SAVE FAILED: " + lerr);
                  notice_until = vehicle.time() + 3.0;
                  app.audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kCancel);
                } else {
                  // P0-G6: Restore pre-edit paint & saved .liv livery cleanly (clearing any unsaved preview edits/vinyls).
                  ShowroomOptions orig = host.build_car_options();
                  std::string rerr;
                  host.showroom.reload_car(app.bake_dir, app.car, orig, rerr);
                  if (!reapply_active_car_livery(app, host)) {
                    host.showroom.preview_colour(paint_before);
                  }
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                app.hub_orbit_dragging = false;
                app.hub_orbit_dragged = false;
                livery_editor.close();
                paint_open = false;
              };
              auto do_undo = [&]() {
                if (livery_editor.undo()) {
                  sync_livery_preview(!livery_editor.document().layers.empty());
                  livery_editor.mark_vinyl_preview_uploaded(double(SDL_GetTicksNS()) * 1e-9);
                  notice = "UNDID LAST EDIT (" + std::to_string(livery_editor.undo_depth()) + " LEFT)";
                  notice_until = vehicle.time() + 2.0;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
              };
              auto do_reset_factory = [&]() {
                livery_editor.reset_to_factory_paint();
                sync_livery_preview(true);
                livery_editor.mark_vinyl_preview_uploaded(double(SDL_GetTicksNS()) * 1e-9);
                notice = "RESET TO FACTORY PAINT";
                notice_until = vehicle.time() + 2.5;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto toggle_catalog = [&]() {
                if (!livery_editor.catalog_open()) {
                  livery_editor.scan_design_catalog(liveries_dir());
                }
                livery_editor.toggle_catalog();
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto save_catalog_copy = [&]() {
                const auto dir = liveries_dir();
                livery_editor.scan_design_catalog(dir);
                const std::string dname =
                    app.car + "_design_" + std::to_string(livery_editor.design_catalog().size() + 1);
                std::string cerr;
                if (livery_editor.save_to_catalog(dir, dname, &cerr)) {
                  notice = "SAVED TO DESIGN CATALOG: " + dname;
                  notice_until = vehicle.time() + 2.5;
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else {
                  notice = "CATALOG SAVE FAILED: " + cerr;
                  notice_until = vehicle.time() + 2.5;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
              };
              auto load_catalog_selected = [&]() {
                std::string cerr;
                if (livery_editor.load_selected_design(&cerr)) {
                  sync_livery_preview(!livery_editor.document().layers.empty());
                  livery_editor.mark_vinyl_preview_uploaded(double(SDL_GetTicksNS()) * 1e-9);
                  livery_editor.set_catalog_open(false);
                  notice = "LOADED DESIGN FROM CATALOG";
                  notice_until = vehicle.time() + 2.5;
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if (!cerr.empty()) {
                  notice = "LOAD FAILED: " + cerr;
                  notice_until = vehicle.time() + 2.5;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
              };

              // Design Catalog modal overlay input.
              if (livery_editor.catalog_open()) {
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                  const SDL_Keycode k = e.key.key;
                  if (k == SDLK_ESCAPE || k == SDLK_F2) {
                    livery_editor.set_catalog_open(false);
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if (k == SDLK_UP || k == SDLK_W) {
                    livery_editor.move_design_selection(-1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (k == SDLK_DOWN || k == SDLK_S) {
                    if (k == SDLK_S && (e.key.mod & SDL_KMOD_CTRL)) {
                      save_catalog_copy();
                    } else {
                      livery_editor.move_design_selection(1);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    }
                  } else if (k == SDLK_C) {
                    save_catalog_copy();
                  } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                    load_catalog_selected();
                  }
                } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                  const auto b = e.gbutton.button;
                  if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_BACK) {
                    livery_editor.set_catalog_open(false);
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                    livery_editor.move_design_selection(-1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                    livery_editor.move_design_selection(1);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                    save_catalog_copy();
                  } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                    load_catalog_selected();
                  }
                } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0 &&
                      size_t(hz.index) < livery_editor.design_catalog().size() &&
                      livery_editor.selected_design() != size_t(hz.index)) {
                    livery_editor.select_design_index(size_t(hz.index));
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                  if (e.button.button == SDL_BUTTON_RIGHT) {
                    livery_editor.set_catalog_open(false);
                    app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  } else if (e.button.button == SDL_BUTTON_LEFT) {
                    carview::ui::HitZone hz{};
                    if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0 &&
                        size_t(hz.index) < livery_editor.design_catalog().size()) {
                      livery_editor.select_design_index(size_t(hz.index));
                      load_catalog_selected();
                    }
                  }
                } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                  livery_editor.move_design_selection(e.wheel.y > 0 ? -1 : 1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
                return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
                        e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
              }

              auto step_tab = [&](int dir) {
                livery_editor.move_tab(dir);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto step_region = [&](int dir) {
                livery_editor.move_region(dir);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto step_lr = [&](int dir) {
                if (livery_editor.tab() == carview::EditorTab::kPaint) {
                  if (livery_editor.region() == carview::PaintRegion::kWindowTint) {
                    livery_editor.step_window_tint(0.10f * float(dir));
                  } else {
                    livery_editor.step_hsv(float(dir) / 18.0f, 0.0f, 0.0f);
                  }
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kManufacturerColors) {
                  livery_editor.move_manufacturer_color(dir);
                  livery_editor.apply_manufacturer_color();
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                  livery_editor.move_special_color(dir);
                  livery_editor.apply_special_color();
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kPreviousColors) {
                  livery_editor.move_previous_color(dir);
                  livery_editor.apply_previous_color();
                  sync_livery_preview(false);
                } else {
                  livery_editor.select_layer(dir);
                }
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              auto step_ud = [&](int dir) {
                if (livery_editor.tab() == carview::EditorTab::kPaint) {
                  if (livery_editor.region() == carview::PaintRegion::kWindowTint) {
                    livery_editor.step_window_tint(-0.10f * float(dir));
                  } else {
                    livery_editor.step_hsv(0.0f, 0.0f, -0.15f * float(dir));
                  }
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kManufacturerColors) {
                  livery_editor.move_manufacturer_color(dir);
                  livery_editor.apply_manufacturer_color();
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                  livery_editor.move_special_color(dir);
                  livery_editor.apply_special_color();
                  sync_livery_preview(false);
                } else if (livery_editor.tab() == carview::EditorTab::kPreviousColors) {
                  livery_editor.move_previous_color(dir);
                  livery_editor.apply_previous_color();
                  sync_livery_preview(false);
                } else {
                  livery_editor.cycle_layer_shape(dir);
                  sync_livery_preview(true);
                }
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
                if (ctrl && k == SDLK_Z && !e.key.repeat) {
                  do_undo();
                } else if (ctrl && k == SDLK_D && !e.key.repeat &&
                           livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                  if (livery_editor.duplicate_selected_layer()) {
                    sync_livery_preview(true);
                    notice = "LAYER DUPLICATED";
                    notice_until = vehicle.time() + 2.0;
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  }
                } else if (k == SDLK_F2 && !e.key.repeat) {
                  toggle_catalog();
                } else if ((k == SDLK_F5 || k == SDLK_BACKSPACE) && !e.key.repeat) {
                  do_reset_factory();
                } else if (k == SDLK_ESCAPE && !e.key.repeat) close_livery(false);
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) close_livery(true);
                else if (k == SDLK_Q) step_tab(-1);
                else if (k == SDLK_E || k == SDLK_TAB) step_tab(1);
                else if (k == SDLK_LEFTBRACKET) step_region(-1);
                else if (k == SDLK_RIGHTBRACKET) step_region(1);
                else if (k == SDLK_F && !e.key.repeat && livery_editor.tab() == carview::EditorTab::kPaint) {
                  livery_editor.move_finish(1);
                  sync_livery_preview(false);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_LEFT || k == SDLK_A) step_lr(-1);
                else if (k == SDLK_RIGHT || k == SDLK_D) step_lr(1);
                else if (k == SDLK_UP || k == SDLK_W) step_ud(-1);
                else if (k == SDLK_DOWN || k == SDLK_S) step_ud(1);
                else if (k == SDLK_Y && !e.key.repeat) {
                  if (livery_editor.tab() != carview::EditorTab::kVinylLayers)
                    livery_editor.set_tab(carview::EditorTab::kVinylLayers);
                  livery_editor.add_vinyl_layer();
                  sync_livery_preview(true);
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                } else if ((k == SDLK_X || k == SDLK_DELETE) && !e.key.repeat) {
                  if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                    livery_editor.delete_selected_layer();
                    sync_livery_preview(true);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else {
                    do_reset_factory();
                  }
                } else if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                  if (k == SDLK_J) { livery_editor.transform_layer(-0.04f, 0, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_L) { livery_editor.transform_layer(0.04f, 0, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_I) { livery_editor.transform_layer(0, -0.04f, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_K) { livery_editor.transform_layer(0, 0.04f, 0, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_R) { livery_editor.transform_layer(0, 0, 0, 0, 15.0f); sync_livery_preview(true); }
                  else if (k == SDLK_EQUALS || k == SDLK_PLUS) { livery_editor.transform_layer(0, 0, 0.05f, 0.05f, 0); sync_livery_preview(true); }
                  else if (k == SDLK_MINUS) { livery_editor.transform_layer(0, 0, -0.05f, -0.05f, 0); sync_livery_preview(true); }
                  else if (k == SDLK_P) { livery_editor.cycle_layer_plane(1); sync_livery_preview(true); }
                  else if (k == SDLK_H) { livery_editor.recolor_layer_hsv(1.0f / 12.0f, 0, 0); sync_livery_preview(true); }
                  else if (k == SDLK_F && !e.key.repeat) { livery_editor.toggle_layer_flip(true); sync_livery_preview(true); }
                  else if (k == SDLK_G && !e.key.repeat) { livery_editor.toggle_layer_flip(false); sync_livery_preview(true); }
                  else if (k == SDLK_U) { livery_editor.adjust_layer_skew(-0.15f); sync_livery_preview(true); }
                  else if (k == SDLK_O) { livery_editor.adjust_layer_skew(0.15f); sync_livery_preview(true); }
                  else if (k == SDLK_N) { livery_editor.adjust_layer_alpha(-0.10f); sync_livery_preview(true); }
                  else if (k == SDLK_B) { livery_editor.adjust_layer_alpha(0.10f); sync_livery_preview(true); }
                  else if (k == SDLK_COMMA) { livery_editor.move_layer_order(-1); sync_livery_preview(true); }
                  else if (k == SDLK_PERIOD) { livery_editor.move_layer_order(1); sync_livery_preview(true); }
                  else if (k == SDLK_T && !e.key.repeat) { livery_editor.toggle_layer_mask(); sync_livery_preview(true); }
                  else if (k == SDLK_0 && !e.key.repeat) {
                    livery_editor.toggle_layer_group();
                    notice = "TOGGLED VINYL GROUP";
                    notice_until = vehicle.time() + 2.0;
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                  else if (k == SDLK_Z && !e.key.repeat) { livery_editor.cycle_shape_category(1); sync_livery_preview(true); }
                  else if (k == SDLK_M && !e.key.repeat) {
                    if (livery_editor.mirror_selected_layer()) {
                      sync_livery_preview(true);
                      notice = "LAYER MIRRORED ACROSS CAR";
                      notice_until = vehicle.time() + 2.5;
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    }
                  } else if (k == SDLK_C && !e.key.repeat) {
                    if (livery_editor.copy_selected_layer()) {
                      notice = "LAYER COPIED TO CLIPBOARD";
                      notice_until = vehicle.time() + 2.0;
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    }
                  } else if (k == SDLK_V && !e.key.repeat) {
                    if (livery_editor.paste_layer()) {
                      sync_livery_preview(true);
                      notice = "LAYER PASTED";
                      notice_until = vehicle.time() + 2.0;
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                const bool vinyl_tab = (livery_editor.tab() == carview::EditorTab::kVinylLayers);
                const bool lt = any_pad_trigger_held(pads, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
                const bool rt = any_pad_trigger_held(pads, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) close_livery(false);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  if (lt) toggle_catalog();
                  else close_livery(true);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  if (vinyl_tab && livery_editor.can_undo()) do_undo();
                  else step_tab(-1);
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) step_tab(1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
                  if (vinyl_tab && rt) { livery_editor.move_layer_order(-1); sync_livery_preview(true); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  else step_lr(-1);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                  if (vinyl_tab && rt) { livery_editor.move_layer_order(1); sync_livery_preview(true); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  else step_lr(1);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  if (vinyl_tab && rt) { livery_editor.adjust_layer_alpha(0.10f); sync_livery_preview(true); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  else step_ud(-1);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  if (vinyl_tab && rt) { livery_editor.adjust_layer_alpha(-0.10f); sync_livery_preview(true); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  else step_ud(1);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_STICK && vinyl_tab) {
                  if (lt) livery_editor.toggle_layer_flip(false);
                  else livery_editor.cycle_layer_plane(1);
                  sync_livery_preview(true);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_STICK && vinyl_tab) {
                  if (lt) {
                    livery_editor.toggle_layer_group();
                    notice = "TOGGLED VINYL GROUP";
                    notice_until = vehicle.time() + 2.0;
                  } else {
                    livery_editor.cycle_shape_category(1);
                    sync_livery_preview(true);
                  }
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_BACK) {
                  if (vinyl_tab) {
                    if (lt) {
                      livery_editor.toggle_layer_mask();
                      sync_livery_preview(true);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (livery_editor.mirror_selected_layer()) {
                      sync_livery_preview(true);
                      notice = "LAYER MIRRORED ACROSS CAR";
                      notice_until = vehicle.time() + 2.5;
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    }
                  } else {
                    toggle_catalog();
                  }
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                  if (vinyl_tab) {
                    if (lt) {
                      if (livery_editor.duplicate_selected_layer()) {
                        sync_livery_preview(true);
                        notice = "LAYER DUPLICATED";
                        notice_until = vehicle.time() + 2.0;
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      }
                    } else {
                      livery_editor.add_vinyl_layer();
                      sync_livery_preview(true);
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    }
                  } else {
                    step_region(1);
                  }
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  if (vinyl_tab) {
                    if (lt) livery_editor.toggle_layer_flip(true);
                    else livery_editor.delete_selected_layer();
                    sync_livery_preview(true);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else {
                    do_reset_factory();
                  }
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                  const bool is_stick =
                      (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ||
                       e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY);
                  if (is_stick) {
                    const float v = float(e.gaxis.value) / 32767.0f;
                    const double now_s = double(SDL_GetTicksNS()) * 1e-9;
                    const bool lt = any_pad_trigger_held(pads, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
                    if (std::fabs(v) > 0.55f) {
                      if (lt) {
                        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) livery_editor.adjust_layer_skew(v * 0.08f);
                        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) livery_editor.adjust_layer_alpha(-v * 0.05f);
                        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX) livery_editor.recolor_layer_hsv(v * 0.03f, 0, 0);
                        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY) livery_editor.recolor_layer_hsv(0, 0, -v * 0.04f);
                        livery_editor.mark_vinyl_preview_dirty(now_s);
                      } else {
                        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
                          livery_editor.transform_layer_continuous(v * 0.025f, 0, 0, 0, 0, now_s);
                        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
                          livery_editor.transform_layer_continuous(0, v * 0.025f, 0, 0, 0, now_s);
                        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX)
                          livery_editor.transform_layer_continuous(0, 0, 0, 0, v * 8.0f, now_s);
                        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY)
                          livery_editor.transform_layer_continuous(0, 0, -v * 0.03f, -v * 0.03f, 0, now_s);
                      }
                      // Throttle GPU texture upload while dragging so vkDeviceWaitIdle never stalls every tick.
                      if (livery_editor.needs_vinyl_preview_upload(now_s)) {
                        sync_livery_preview(true);
                        livery_editor.mark_vinyl_preview_uploaded(now_s);
                      }
                    } else if (std::fabs(v) <= 0.35f && livery_editor.has_pending_vinyl_preview()) {
                      sync_livery_preview(true);
                      livery_editor.mark_vinyl_preview_uploaded(now_s);
                    }
                  }
                } else {
                  handle_hub_orbit_input(app, hub_cam, e, false, true);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                if (handle_hub_orbit_input(app, hub_cam, e, false, false)) {
                  return true;
                }
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                  if (livery_editor.tab() == carview::EditorTab::kManufacturerColors &&
                      size_t(hz.index) < livery_editor.manufacturer_colors().size() &&
                      livery_editor.selected_manufacturer_color() != size_t(hz.index)) {
                    livery_editor.select_manufacturer_index(size_t(hz.index));
                    livery_editor.apply_manufacturer_color();
                    sync_livery_preview(false);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors &&
                             size_t(hz.index) < livery_editor.special_colors().size() &&
                             livery_editor.selected_special() != size_t(hz.index)) {
                    livery_editor.select_special_index(size_t(hz.index));
                    livery_editor.apply_special_color();
                    sync_livery_preview(false);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (livery_editor.tab() == carview::EditorTab::kPreviousColors &&
                             size_t(hz.index) < livery_editor.previous_colors().size() &&
                             livery_editor.selected_previous_color() != size_t(hz.index)) {
                    livery_editor.select_previous_index(size_t(hz.index));
                    livery_editor.apply_previous_color();
                    sync_livery_preview(false);
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  } else if (livery_editor.tab() == carview::EditorTab::kVinylLayers &&
                             size_t(hz.index) < livery_editor.document().layers.size() &&
                             livery_editor.selected_layer() != size_t(hz.index)) {
                    livery_editor.select_layer_index(size_t(hz.index));
                    app.audio_host.trigger_ui(audio::UiCue::kMove);
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  close_livery(false);
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                  handle_hub_orbit_input(app, hub_cam, e, false, false);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (!ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    handle_hub_orbit_input(app, hub_cam, e, false, false);
                  } else {
                    if (hz.group == carview::ui::kHitTab && hz.index >= 0 && hz.index < int(carview::EditorTab::kCount)) {
                      livery_editor.set_tab(carview::EditorTab(hz.index));
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui4::kHitRegion && hz.index >= 0 &&
                               hz.index < int(carview::PaintRegion::kCount)) {
                      livery_editor.set_region(carview::PaintRegion(hz.index));
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui::kHitStepLeft || hz.group == carview::ui::kHitStepRight) {
                      const int dir = (hz.group == carview::ui::kHitStepLeft) ? -1 : +1;
                      if (livery_editor.region() == carview::PaintRegion::kWindowTint) {
                        livery_editor.step_window_tint(0.10f * float(dir));
                      } else {
                        livery_editor.move_finish(dir);
                      }
                      sync_livery_preview(false);
                      app.audio_host.trigger_ui(audio::UiCue::kMove);
                    } else if (hz.group == carview::ui4::kHitSwatch && hz.index >= 0) {
                      livery_editor.set_swatch(hz.index / 18, hz.index % 18, 4, 18);
                      sync_livery_preview(false);
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    } else if (hz.group == carview::ui::kHitRow && hz.index >= 0) {
                      if (livery_editor.tab() == carview::EditorTab::kManufacturerColors) {
                        livery_editor.select_manufacturer_index(size_t(hz.index));
                        livery_editor.apply_manufacturer_color();
                        sync_livery_preview(false);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                        livery_editor.select_special_index(size_t(hz.index));
                        livery_editor.apply_special_color();
                        sync_livery_preview(false);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      } else if (livery_editor.tab() == carview::EditorTab::kPreviousColors) {
                        livery_editor.select_previous_index(size_t(hz.index));
                        livery_editor.apply_previous_color();
                        sync_livery_preview(false);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      } else if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                        if (size_t(hz.index) < livery_editor.document().layers.size()) {
                          livery_editor.select_layer_index(size_t(hz.index));
                          app.audio_host.trigger_ui(audio::UiCue::kMove);
                        } else {
                          livery_editor.add_vinyl_layer();
                          sync_livery_preview(true);
                          app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        }
                      }
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) close_livery(true);
                      else if (hz.index == 1) {
                        if (livery_editor.tab() == carview::EditorTab::kVinylLayers && livery_editor.can_undo()) {
                          do_undo();
                        } else {
                          step_tab(1);
                        }
                      } else if (hz.index == 2) {
                        step_lr(1);
                      } else if (hz.index == 3) {
                        if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                          livery_editor.add_vinyl_layer();
                          sync_livery_preview(true);
                          app.audio_host.trigger_ui(audio::UiCue::kAccept);
                        } else {
                          step_region(1);
                        }
                      } else if (hz.index == 4) {
                        if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                          livery_editor.delete_selected_layer();
                          sync_livery_preview(true);
                          app.audio_host.trigger_ui(audio::UiCue::kMove);
                        } else {
                          do_reset_factory();
                        }
                      } else if (hz.index == 5) {
                        close_livery(false);
                      }
                    }
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                handle_hub_orbit_input(app, hub_cam, e, false, false);
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                step_lr(e.wheel.y > 0 ? -1 : +1);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (paint_open) {
              auto pick = [&](int by) {
                const int n = int(paint_order.size());
                paint_pick = size_t(((int(paint_pick) + by) % n + n) % n);
                host.showroom.preview_colour(paint_order[paint_pick]);
              };
              auto leave = [&](bool keep) {
                paint_open = false;
                app.hub_orbit_dragging = false;
                app.hub_orbit_dragged = false;
                const size_t chosen = paint_order[paint_pick];
                if (!keep || chosen == paint_before) {
                  host.showroom.preview_colour(paint_before);
                  return;
                }
                app.paint_sequence = host.showroom.colours()[chosen].sequence;
                app.paint_car = app.car;
                if (app.have_garage)
                  if (const garage::Car* sel = app.career_garage.selected_car())
                    if (garage::Car* gc = app.career_garage.find(sel->id);
                        gc && app.baked_ids.count(gc->car_id) && lower_ascii(app.baked_ids.at(gc->car_id)) == lower_ascii(app.car)) {
                      gc->colour = int(app.paint_sequence);
                      app.write_career_garage();
                    }
                save_now("paint");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
                app.have_spawn = true;
                app.next_car = app.car;
                running = false;
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) leave(false);
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat) leave(true);
                else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) pick(-1);
                else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) pick(1);
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) leave(false);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) leave(true);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) pick(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) pick(1);
              }
              if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                handle_hub_orbit_input(app, hub_cam, e, false, true);
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                handle_hub_orbit_input(app, hub_cam, e, false, false);
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) { app.audio_host.trigger_ui(audio::UiCue::kCancel); leave(false); }
                else if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE) {
                  handle_hub_orbit_input(app, hub_cam, e, false, false);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                const bool was_dragging = app.hub_orbit_dragging;
                const bool moved = handle_hub_orbit_input(app, hub_cam, e, false, false);
                if (e.button.button == SDL_BUTTON_LEFT && was_dragging && !moved) {
                  app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  leave(true);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                pick(e.wheel.y > 0 ? -1 : +1);
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            slice12.on_sdl_event(e, pads);
  return false;
}

}  // namespace carview
