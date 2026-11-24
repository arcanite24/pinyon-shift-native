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

bool DriveSession::handle_hub_shop_event(const SDL_Event& e) {
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
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) {
                  garage_open = false;
                  garage_sell_confirm_id = -1;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) {
                  move_garage(-1);
                } else if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) {
                  move_garage(1);
                } else if (k == SDLK_Y && !e.key.repeat && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_sort(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (k == SDLK_X && !e.key.repeat && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_class_filter(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
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
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                  move_garage(-1);
                } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                  move_garage(1);
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_sort(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_WEST && use_browser) {
                  garage_sell_confirm_id = -1;
                  garage_browser.cycle_class_filter(1);
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                  try_sell();
                } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
                  drive_selected();
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION && n > 0) {
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
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT && n > 0) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui::kHitRow &&
                      hz.index >= 0 && size_t(hz.index) < n) {
                    if (use_browser) garage_browser.select_index(size_t(hz.index));
                    else garage_pick = size_t(hz.index);
                    drive_selected();
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
              auto leave_shop = [&]() {
                shop.close();
                if (!shop.changed()) return;
                app.write_career_garage();
                save_now("upgrade");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
                app.have_spawn = true;
                app.next_car = app.car;
                running = false;
              };
              auto fit = [&]() {
                std::string message;
                const bool ok = shop.fit(discount(), app.career.credits, message);
                app.audio_host.trigger_ui(ok ? audio::UiCue::kPurchase : audio::UiCue::kError);
                notice = message;
                notice_until = vehicle.time() + 3;
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) { app.audio_host.trigger_ui(audio::UiCue::kCancel); leave_shop(); }
                if (k == SDLK_UP || k == SDLK_W) { shop.move_slot(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_DOWN || k == SDLK_S) { shop.move_slot(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_LEFT || k == SDLK_A) { shop.move_part(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_RIGHT || k == SDLK_D) { shop.move_part(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_Q) { shop.move_category(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_E) { shop.move_category(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (k == SDLK_X) {
                  std::string smsg;
                  const bool ok = shop.sell_shown_part(app.career.credits, smsg);
                  app.audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kCancel);
                  notice = smsg, notice_until = vehicle.time() + 3;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { fit(); }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  leave_shop();
                }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) { shop.move_slot(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { shop.move_slot(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) { shop.move_part(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) { shop.move_part(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { shop.move_category(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { shop.move_category(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  std::string smsg;
                  const bool ok = shop.sell_shown_part(app.career.credits, smsg);
                  app.audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kCancel);
                  notice = smsg, notice_until = vehicle.time() + 3;
                }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) { fit(); }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                  const size_t cur_sel = shop.category() == carview::ShopCategory::kTuning
                                             ? shop.selected_slider()
                                             : shop.selected_row();
                  if (cur_sel != size_t(hz.index)) {
                    shop.select_row(size_t(hz.index));
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
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The Autoshow takes the keys and buttons while open; a purchase
            // drives the new car away from where this one stands.
            if (autoshow.is_open()) {
              auto buy = [&]() {
                const auto db = app.parts_db.valid() ? app.parts_db.get() : nullptr;
                std::string media, message = "no parts database";
                const bool bought = db && autoshow.buy(*db, app.career_garage, app.career.credits, media, message);
                notice = message;
                notice_until = vehicle.time() + 4;
                if (!bought) {
                  app.audio_host.trigger_ui(audio::UiCue::kError);
                  return;
                }
                app.audio_host.trigger_ui(audio::UiCue::kPurchase);
                app.audio_host.trigger_ui(audio::UiCue::kCarAdded);
                autoshow.close();
                app.garage_picker();
                app.write_career_garage();
                save_now("purchase");
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
                app.have_spawn = true;
                app.next_car = media;
                running = false;
              };
              auto confirm_step = [&]() {
                if (autoshow.advance_step()) buy();
              };
              auto cancel_step = [&]() {
                if (!autoshow.back_step()) autoshow.close();
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                  if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) { autoshow.move_make(-1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) { autoshow.move_make(1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                  if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) { autoshow.move_colour(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) { autoshow.move_colour(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                } else {
                  if (k == SDLK_LEFT || k == SDLK_A || k == SDLK_UP || k == SDLK_W) { autoshow.move(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (k == SDLK_RIGHT || k == SDLK_D || k == SDLK_DOWN || k == SDLK_S) { autoshow.move(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (k == SDLK_Q) { autoshow.move_make(-1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (k == SDLK_E) { autoshow.move_make(1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (k == SDLK_C) { autoshow.set_step(carview::AutoshowStep::kColorSelect); app.audio_host.trigger_ui(audio::UiCue::kAccept); }
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { app.audio_host.trigger_ui(audio::UiCue::kAccept); confirm_step(); }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) { app.audio_host.trigger_ui(audio::UiCue::kCancel); cancel_step(); }
                if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) { autoshow.move_make(-1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { autoshow.move_make(1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) { autoshow.move_colour(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { autoshow.move_colour(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                } else {
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP) { autoshow.move(-1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { autoshow.move(1); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { autoshow.move_make(-1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { autoshow.move_make(1, app.career.credits); app.audio_host.trigger_ui(audio::UiCue::kMove); }
                  if (b == SDL_GAMEPAD_BUTTON_NORTH) { autoshow.set_step(carview::AutoshowStep::kColorSelect); app.audio_host.trigger_ui(audio::UiCue::kAccept); }
                }
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) { app.audio_host.trigger_ui(audio::UiCue::kAccept); confirm_step(); }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
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
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  cancel_step();
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                    if (autoshow.step() == carview::AutoshowStep::kMakeSelect) {
                      autoshow.select_make(size_t(hz.index), app.career.credits);
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      confirm_step();
                    } else if (autoshow.step() == carview::AutoshowStep::kCarSelect) {
                      autoshow.select_index(size_t(hz.index));
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      confirm_step();
                    }
                  } else if (autoshow.step() == carview::AutoshowStep::kColorSelect) {
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                    confirm_step();
                  }
                }
              }
              if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                const int dir = e.wheel.y > 0 ? -1 : 1;
                if (autoshow.step() == carview::AutoshowStep::kMakeSelect) autoshow.move_make(dir, app.career.credits);
                else if (autoshow.step() == carview::AutoshowStep::kColorSelect) autoshow.move_colour(dir);
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
                app.rally_session.retire();
                app.rally_hub_open = true;
                app.leave_race = true;
                running = false;
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            if (app.rally_service_open) {
              auto act_service = [&]() {
                if (app.rally_service_selected == 0) {
                  app.rally_service_open = false;
                  if (app.rally_session.advance_from_service_area()) {
                    if (const auto* stg = app.rally_session.current_stage()) {
                      app.race_options.stage_title = stg->name;
                      app.next_event = "stage:" + std::to_string(stg->route_id);
                      running = false;
                    }
                  }
                } else if (app.rally_service_selected == 1) {
                  car_damage.reset();
                  app.rally_service_repaired = true;
                  app.rally_notice = "CAR REPAIRED TO 100% — READY FOR NEXT STAGE";
                } else if (app.rally_service_selected == 2) {
                  app.rally_service_open = false;
                  open_shop_or_autoshow(false);
                } else if (app.rally_service_selected == 3) {
                  app.rally_service_open = false;
                  app.rally_session.retire();
                  app.rally_hub_open = true;
                  app.leave_race = true;
                  running = false;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_UP || e.key.key == SDLK_W) { app.rally_service_selected = (app.rally_service_selected + 3) % 4; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_S) { app.rally_service_selected = (app.rally_service_selected + 1) % 4; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE) { app.audio_host.trigger_ui(audio::UiCue::kAccept); act_service(); }
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) { app.rally_service_selected = (app.rally_service_selected + 3) % 4; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { app.rally_service_selected = (app.rally_service_selected + 1) % 4; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) { app.audio_host.trigger_ui(audio::UiCue::kAccept); act_service(); }
              } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow &&
                    hz.index >= 0 && hz.index < 4 && app.rally_service_selected != hz.index) {
                  app.rally_service_selected = hz.index;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  app.rally_service_selected = 3;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                  act_service();
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && hz.index >= 0 && hz.index < 4) {
                      app.rally_service_selected = hz.index;
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      act_service();
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) { app.audio_host.trigger_ui(audio::UiCue::kAccept); act_service(); }
                      else if (hz.index == 2) { app.rally_service_selected = 3; app.audio_host.trigger_ui(audio::UiCue::kCancel); act_service(); }
                    }
                  }
                }
              } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) app.rally_service_selected = (app.rally_service_selected + 3) % 4;
                else if (e.wheel.y < 0) app.rally_service_selected = (app.rally_service_selected + 1) % 4;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
  return false;
}

bool DriveSession::handle_rally_career_club_event(const SDL_Event& e) {
            if (app.rally_hub_open && app.rally_catalog.loaded) {
              int64_t cid = 0;
              int clid = 4;
              current_car_info(cid, clid);
              const int disc = host.free_roam ? freeroam::upgrade_discount(host.free_roam->signs_found()) : 0;
              const auto db_ptr = app.parts_db.valid() ? app.parts_db.get() : std::shared_ptr<cars::PartsDb>{};
              const auto hst = carview::make_rally_hub_state(app.rally_catalog, app.saved, app.have_garage ? &app.career_garage : nullptr,
                                                             db_ptr.get(), app.class_bands, app.car, clid, 0, disc,
                                                             app.rally_hub_selected, app.rally_notice);
              const int n_items = std::max(1, int(hst.items.size()));
              auto activate_rally = [&]() {
                if (app.rally_hub_selected < 0 || size_t(app.rally_hub_selected) >= hst.items.size()) return;
                const auto& it = hst.items[size_t(app.rally_hub_selected)];
                if (it.kind == carview::RallyHubItem::kHeats) {
                  if (app.rally_session.start_heats(app.rally_catalog, int(app.race_options.difficulty), app.race_options.seed)) {
                    if (auto bit = app.baked_ids.find(app.rally_catalog.heats.player_car_id); bit != app.baked_ids.end()) {
                      app.pre_loaner_car = app.car;
                      app.pre_loaner_garage_id = app.career_garage.selected;
                      app.car = bit->second;
                    }
                    app.rally_hub_open = false;
                    app.race_options.stage_title = app.rally_catalog.heats.name;
                    app.next_event = "stage:" + std::to_string(app.rally_catalog.heats.route_id);
                    running = false;
                  }
                } else if (it.kind == carview::RallyHubItem::kChampionship) {
                  if (!it.unlocked || !it.eligible) {
                    app.rally_notice = it.lock_reason;
                    return;
                  }
                  if (app.rally_session.start_championship(app.rally_catalog, it.event_id, int(app.race_options.difficulty),
                                                       app.race_options.seed)) {
                    if (const auto* stg = app.rally_session.current_stage()) {
                      app.rally_hub_open = false;
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
                  app.rally_hub_open = false;
                } else if (it.kind == carview::RallyHubItem::kReturnColorado) {
                  app.rally_hub_open = false;
                  app.rally_world_switch = -1;
                  running = false;
                }
              };
              if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_ESCAPE) { app.rally_hub_open = false; app.audio_host.trigger_ui(audio::UiCue::kCancel); }
                else if (e.key.key == SDLK_UP || e.key.key == SDLK_W) { app.rally_hub_selected = (app.rally_hub_selected + n_items - 1) % n_items; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_S) { app.rally_hub_selected = (app.rally_hub_selected + 1) % n_items; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE) { app.audio_host.trigger_ui(audio::UiCue::kAccept); activate_rally(); }
              } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) { app.rally_hub_open = false; app.audio_host.trigger_ui(audio::UiCue::kCancel); }
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) { app.rally_hub_selected = (app.rally_hub_selected + n_items - 1) % n_items; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) { app.rally_hub_selected = (app.rally_hub_selected + 1) % n_items; app.audio_host.trigger_ui(audio::UiCue::kMove); }
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) { app.audio_host.trigger_ui(audio::UiCue::kAccept); activate_rally(); }
              } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow &&
                    hz.index >= 0 && hz.index < n_items && app.rally_hub_selected != hz.index) {
                  app.rally_hub_selected = hz.index;
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                  app.rally_hub_open = false;
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
                    if (hz.group == carview::ui::kHitRow && hz.index >= 0 && hz.index < n_items) {
                      app.rally_hub_selected = hz.index;
                      app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      activate_rally();
                    } else if (hz.group == carview::ui4::kHitButton) {
                      if (hz.index == 0) { app.audio_host.trigger_ui(audio::UiCue::kAccept); activate_rally(); }
                      else if (hz.index == 2) { app.rally_hub_open = false; app.audio_host.trigger_ui(audio::UiCue::kCancel); }
                    }
                  }
                }
              } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (e.wheel.y > 0) app.rally_hub_selected = (app.rally_hub_selected + n_items - 1) % n_items;
                else if (e.wheel.y < 0) app.rally_hub_selected = (app.rally_hub_selected + 1) % n_items;
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              }
              return (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED && e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED);
            }
            // The 1000 Club screen: up/down inspects challenges & hints, enter begins challenges in this car.
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
                notice = "1000 CLUB: CHALLENGES BEGUN IN THIS CAR";
                notice_until = vehicle.time() + 4;
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
            // Paint Shop & Vinyl/Livery Editor (LiveryEditor live preview + .liv persistence; legacy swatch fallback).
            if (paint_open && livery_editor.is_open()) {
              auto close_livery = [&](bool save_changes) {
                if (save_changes) {
                  std::string lerr;
                  const bool ok = livery_editor.save(livery_edit_path, &lerr);
                  sync_livery_preview(!livery_editor.document().layers.empty());
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
                  save_now("livery");
                  notice = ok ? "LIVERY SAVED & APPLIED" : ("LIVERY SAVE FAILED: " + lerr);
                  notice_until = vehicle.time() + 3.0;
                  app.audio_host.trigger_ui(ok ? audio::UiCue::kAccept : audio::UiCue::kCancel);
                } else {
                  host.showroom.preview_colour(paint_before);
                  app.audio_host.trigger_ui(audio::UiCue::kCancel);
                }
                livery_editor.close();
                paint_open = false;
              };
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
                } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                  livery_editor.move_special_color(dir);
                  livery_editor.apply_special_color();
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
                } else if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                  livery_editor.move_special_color(dir);
                  livery_editor.apply_special_color();
                  sync_livery_preview(false);
                } else {
                  livery_editor.cycle_layer_shape(dir);
                  sync_livery_preview(true);
                }
                app.audio_host.trigger_ui(audio::UiCue::kMove);
              };
              if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && !e.key.repeat) close_livery(false);
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
                  } else {
                    livery_editor.move_finish(1);
                    sync_livery_preview(false);
                  }
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
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
                }
              }
              if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto b = e.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_EAST) close_livery(false);
                else if (b == SDL_GAMEPAD_BUTTON_SOUTH) close_livery(true);
                else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) step_tab(-1);
                else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) step_tab(1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) step_lr(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) step_lr(1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) step_ud(-1);
                else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) step_ud(1);
                else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                  if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                    livery_editor.add_vinyl_layer();
                    sync_livery_preview(true);
                    app.audio_host.trigger_ui(audio::UiCue::kAccept);
                  } else {
                    step_region(1);
                  }
                } else if (b == SDL_GAMEPAD_BUTTON_WEST) {
                  if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                    livery_editor.delete_selected_layer();
                    sync_livery_preview(true);
                  } else {
                    livery_editor.move_finish(1);
                    sync_livery_preview(false);
                  }
                  app.audio_host.trigger_ui(audio::UiCue::kMove);
                }
              }
              if (e.type == SDL_EVENT_MOUSE_MOTION) {
                carview::ui::HitZone hz{};
                if (ui_mouse_hit(e.motion.x, e.motion.y, hz) && hz.group == carview::ui::kHitRow && hz.index >= 0) {
                  if (livery_editor.tab() == carview::EditorTab::kSpecialColors &&
                      size_t(hz.index) < livery_editor.special_colors().size() &&
                      livery_editor.selected_special() != size_t(hz.index)) {
                    livery_editor.select_special_index(size_t(hz.index));
                    livery_editor.apply_special_color();
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
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                  carview::ui::HitZone hz{};
                  if (ui_mouse_hit(e.button.x, e.button.y, hz)) {
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
                      if (livery_editor.tab() == carview::EditorTab::kSpecialColors) {
                        livery_editor.select_special_index(size_t(hz.index));
                        livery_editor.apply_special_color();
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
                      else if (hz.index == 1) step_tab(1);
                      else if (hz.index == 3) {
                        if (livery_editor.tab() != carview::EditorTab::kVinylLayers)
                          livery_editor.set_tab(carview::EditorTab::kVinylLayers);
                        livery_editor.add_vinyl_layer();
                        sync_livery_preview(true);
                        app.audio_host.trigger_ui(audio::UiCue::kAccept);
                      } else if (hz.index == 4) {
                        if (livery_editor.tab() == carview::EditorTab::kVinylLayers) {
                          livery_editor.delete_selected_layer();
                          sync_livery_preview(true);
                        } else {
                          livery_editor.move_finish(1);
                          sync_livery_preview(false);
                        }
                        app.audio_host.trigger_ui(audio::UiCue::kMove);
                      } else if (hz.index == 5) {
                        close_livery(false);
                      }
                    }
                  }
                }
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
              if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) { app.audio_host.trigger_ui(audio::UiCue::kCancel); leave(false); }
                else if (e.button.button == SDL_BUTTON_LEFT) { app.audio_host.trigger_ui(audio::UiCue::kAccept); leave(true); }
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
