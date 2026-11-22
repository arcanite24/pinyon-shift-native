#include "trainer_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui4_chrome.h"

namespace carview {
namespace {

struct CreditChoice {
  const char* label;
  int64_t amount;
};

constexpr CreditChoice kSetCredits[] = {
    {"0 CR", 0},
    {"250,000 CR", 250000},
    {"1,000,000 CR", 1000000},
    {"5,000,000 CR", 5000000},
    {"20,000,000 CR", 20000000},
};
constexpr CreditChoice kAddCredits[] = {
    {"+100,000 CR", 100000},
    {"+1,000,000 CR", 1000000},
    {"+10,000,000 CR", 10000000},
};

constexpr double kGameSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
constexpr double kTodHours[] = {-1.0, 0.0, 3.0, 6.0, 9.0, 12.0, 15.0, 18.0, 21.0};
constexpr double kDayLengths[] = {0.0, 12.0, 24.0, 48.0, 120.0, 360.0, 1440.0, -1.0};
constexpr double kPriceMults[] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0};
constexpr double kRewardMults[] = {0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0};

template <size_t N>
double cycle_double(const double (&arr)[N], double cur, int by) {
  size_t best = 0;
  double best_d = 1e30;
  for (size_t i = 0; i < N; ++i) {
    const double d = std::fabs(arr[i] - cur);
    if (d < best_d) best_d = d, best = i;
  }
  const int next = ((int(best) + by) % int(N) + int(N)) % int(N);
  return arr[size_t(next)];
}

std::string fmt_day_length(double mins) {
  if (mins < 0.0) return "PC CLOCK";
  if (mins <= 0.0) return "GAME (30M)";
  if (mins < 60.0) return std::to_string(int(std::lround(mins))) + " MIN";
  return std::to_string(int(std::lround(mins / 60.0))) + " H";
}

std::string fmt_tod(double h) {
  if (h < 0.0) return "RUNNING";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:00", int(std::lround(h)) % 24);
  return buf;
}

std::string fmt_mult(double m) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2fX", m);
  return buf;
}

}  // namespace

void TrainerOverlay::switch_tab(int by) {
  const int n = int(Tab::kCount);
  tab_ = Tab(((int(tab_) + by) % n + n) % n);
  row_ = 0;
}

void TrainerOverlay::set_tab(Tab t) {
  tab_ = t;
  row_ = 0;
}

int TrainerOverlay::row_count() const {
  switch (tab_) {
    case Tab::kPlayer: return 6;
    case Tab::kWorld: return 6;
    case Tab::kVehicle: return 6;
    case Tab::kDebug: return 6;
    default: return 1;
  }
}

void TrainerOverlay::move(int by) {
  const int n = row_count();
  row_ = ((row_ + by) % n + n) % n;
}

void TrainerOverlay::step(int by, drive::TrainerState& state) {
  if (tab_ == Tab::kPlayer) {
    switch (row_) {
      case 0: {
        const int n = int(std::size(kSetCredits));
        set_credits_idx_ = ((set_credits_idx_ + by) % n + n) % n;
        break;
      }
      case 1: {
        const int n = int(std::size(kAddCredits));
        add_credits_idx_ = ((add_credits_idx_ + by) % n + n) % n;
        break;
      }
      case 2: wristband_idx_ = ((wristband_idx_ + by) % 7 + 7) % 7; break;
      case 3:
        state.pacing.enabled = !state.pacing.enabled;
        break;
      case 4:
        state.pacing.price_multiplier = cycle_double(kPriceMults, state.pacing.price_multiplier, by);
        state.pacing.enabled = true;
        break;
      case 5:
        state.pacing.reward_multiplier = cycle_double(kRewardMults, state.pacing.reward_multiplier, by);
        state.pacing.enabled = true;
        break;
      default: break;
    }
    return;
  }

  if (tab_ == Tab::kWorld) {
    switch (row_) {
      case 0: state.config.game_speed = cycle_double(kGameSpeeds, state.config.game_speed, by); break;
      case 1: state.config.time_of_day_hour = cycle_double(kTodHours, state.config.time_of_day_hour, by); break;
      case 2: state.config.day_length_minutes = cycle_double(kDayLengths, state.config.day_length_minutes, by); break;
      case 3: state.config.free_camera = !state.config.free_camera; break;
      case 4: state.config.reveal_collectibles = !state.config.reveal_collectibles; break;
      case 5: state.config.any_car_in_any_event = !state.config.any_car_in_any_event; break;
      default: break;
    }
    return;
  }

  if (tab_ == Tab::kVehicle) {
    if (row_ == 0) state.config.fast_travel_anywhere = !state.config.fast_travel_anywhere;
    return;
  }

  if (tab_ == Tab::kDebug) {
    switch (row_) {
      case 0: state.hidden_cars = !state.hidden_cars; break;
      case 1: state.config.debug_bloom = !state.config.debug_bloom; break;
      case 2: state.config.debug_motion_blur = !state.config.debug_motion_blur; break;
      case 3: state.config.debug_dof = !state.config.debug_dof; break;
      case 4: state.config.debug_ffb_trace = !state.config.debug_ffb_trace; break;
      case 5: state.config.debug_telemetry_probe = !state.config.debug_telemetry_probe; break;
      default: break;
    }
  }
}

TrainerActionOutcome TrainerOverlay::activate(drive::TrainerState& state, int64_t& credits, int& wristband_points,
                                              int& collected_wristband, double now_s, bool in_race) {
  TrainerActionOutcome out;
  if (!state.config.cheats_enabled) {
    last_message_ = "ENABLE TRAINER IN F6 > PINYON (USES ISOLATED user-modded SAVE)";
    out.message = last_message_;
    return out;
  }
  if (tab_ == Tab::kPlayer) {
    if (row_ == 0) {
      state.apply_set_credits(credits, kSetCredits[size_t(set_credits_idx_)].amount);
      out.credits_changed = true;
      last_message_ = std::string("CREDITS SET TO ") + kSetCredits[size_t(set_credits_idx_)].label;
      out.message = last_message_;
      return out;
    }
    if (row_ == 1) {
      state.apply_add_credits(credits, kAddCredits[size_t(add_credits_idx_)].amount);
      out.credits_changed = true;
      last_message_ = std::string("ADDED ") + kAddCredits[size_t(add_credits_idx_)].label;
      out.message = last_message_;
      return out;
    }
    if (row_ == 2) {
      state.apply_set_wristband_level(wristband_points, collected_wristband, wristband_idx_);
      out.wristband_changed = true;
      last_message_ = std::string("WRISTBAND SET TO ") + drive::wristband_tier_name(wristband_idx_);
      out.message = last_message_;
      return out;
    }
    step(+1, state);
    out.settings_dirty = true;
    return out;
  }

  if (tab_ == Tab::kVehicle) {
    switch (row_) {
      case 0:
        state.config.fast_travel_anywhere = !state.config.fast_travel_anywhere;
        last_message_ = state.config.fast_travel_anywhere ? "FAST TRAVEL ANYWHERE: ON" : "FAST TRAVEL ANYWHERE: OFF";
        out.settings_dirty = true;
        break;
      case 1:
        if (state.request_teleport_to_last_road(now_s, in_race)) {
          last_message_ = "TELEPORT QUEUED TO LAST MAP ROAD (10S WINDOW)";
          out.close_overlay = true;
        } else {
          last_message_ = in_race ? "TELEPORT REJECTED DURING RACE" : "SELECT A ROAD ON THE MAP FIRST";
        }
        break;
      case 2:
        if (state.request_gps_to_last_road(in_race)) {
          last_message_ = "GPS ROUTE SET TO LAST MAP ROAD";
          out.close_overlay = true;
        } else {
          last_message_ = in_race ? "GPS REJECTED DURING RACE" : "SELECT A ROAD ON THE MAP FIRST";
        }
        break;
      case 3:
        if (state.save_last_map_road()) {
          last_message_ = "SAVED MAP ROAD TO SETTINGS";
          out.saved_road_changed = true;
          out.settings_dirty = true;
        } else {
          last_message_ = "SELECT A ROAD ON THE MAP FIRST";
        }
        break;
      case 4:
        if (state.request_gps_to_saved_road(in_race)) {
          last_message_ = "GPS ROUTE SET TO SAVED ROAD";
          out.close_overlay = true;
        } else {
          last_message_ = in_race ? "GPS REJECTED DURING RACE" : "NO SAVED ROAD YET";
        }
        break;
      case 5:
        if (state.request_teleport_to_saved_road(now_s, in_race)) {
          last_message_ = "TELEPORT QUEUED TO SAVED ROAD (10S WINDOW)";
          out.close_overlay = true;
        } else {
          last_message_ = in_race ? "TELEPORT REJECTED DURING RACE" : "NO SAVED ROAD YET";
        }
        break;
      default: break;
    }
    out.message = last_message_;
    return out;
  }

  step(+1, state);
  out.settings_dirty = true;
  return out;
}

std::vector<TrainerOverlay::RowView> TrainerOverlay::rows(const drive::TrainerState& state) const {
  std::vector<RowView> out;
  if (tab_ == Tab::kPlayer) {
    out.push_back({"SET CREDITS", kSetCredits[size_t(set_credits_idx_)].label,
                   "Left/Right picks amount, [A]/Enter sets modded profile credits immediately.", true, true,
                   row_ == 0});
    out.push_back({"ADD CREDITS", kAddCredits[size_t(add_credits_idx_)].label,
                   "Adds credits to the isolated user-modded save.", true, true, row_ == 1});
    out.push_back({"SET WRISTBAND LEVEL",
                   std::string("TIER ") + std::to_string(wristband_idx_ + 1) + " (" +
                       drive::wristband_tier_name(wristband_idx_) + ")",
                   "Unlocks the chosen Horizon Wristband tier and raises wristband points.", true, true, row_ == 2});
    out.push_back({"FESTIVAL PACING", state.pacing.enabled ? "ON" : "OFF",
                   "Scales car/upgrade/rim prices (0.25x-3x) and race/loyalty/XP rewards (0.5x-5x).", true, false,
                   row_ == 3});
    out.push_back({"PACING: PRICES", fmt_mult(state.pacing.price_multiplier),
                   "Multiplier for Autoshow cars, upgrades and rims (DLC keeps stock price).", true, false,
                   row_ == 4});
    out.push_back({"PACING: REWARDS", fmt_mult(state.pacing.reward_multiplier),
                   "Multiplier for race credits, wristband points, driver XP and loyalty XP.", true, false,
                   row_ == 5});
    return out;
  }

  if (tab_ == Tab::kWorld) {
    char sp[16];
    std::snprintf(sp, sizeof(sp), "%.2fX", state.config.game_speed);
    out.push_back({"GAME SPEED", sp, "Scales simulation step rate from 0.25x slow-motion to 2.0x fast-forward.",
                   true, false, row_ == 0});
    out.push_back({"TIME OF DAY", fmt_tod(state.config.time_of_day_hour),
                   "Holds the Colorado sky clock at a chosen hour, or RUNNING for normal progression.", true, false,
                   row_ == 1});
    out.push_back({"DAY LENGTH", fmt_day_length(state.config.day_length_minutes),
                   "Real time per 24 game hours (12 min to 24 h), or sync directly to this PC's clock.", true, false,
                   row_ == 2});
    out.push_back({"FREE CAMERA", state.config.free_camera ? "ON" : "OFF",
                   "Detaches the camera to fly freely over Colorado while driving is paused.", true, false,
                   row_ == 3});
    out.push_back({"REVEAL COLLECTIBLES", state.config.reveal_collectibles ? "ON" : "OFF",
                   "Shows uncollected Discount Signs and Barn Finds on the map and minimap.", true, false,
                   row_ == 4});
    out.push_back({"ANY CAR IN ANY EVENT", state.config.any_car_in_any_event ? "ON" : "OFF",
                   "Overrides EventRestrictions (class, make, year, drivetrain) so any car enters.", true, false,
                   row_ == 5});
    return out;
  }

  if (tab_ == Tab::kVehicle) {
    const bool has_last = state.last_map_road.has_value();
    const bool has_saved = state.config.saved_map_road.has_value();
    out.push_back({"FAST TRAVEL ANYWHERE", state.config.fast_travel_anywhere ? "ON" : "OFF",
                   "Permits fast travel to any road or marker on the map without credit cost.", true, false,
                   row_ == 0});
    out.push_back({"TELEPORT TO LAST MAP ROAD", has_last ? "READY" : "NONE",
                   "Queues road-snapped teleport to the last road selected on the world map.", has_last, true,
                   row_ == 1});
    out.push_back({"GPS TO LAST MAP ROAD", has_last ? "READY" : "NONE",
                   "Routes Colorado GPS sat-nav to the last road chosen on the map.", has_last, true, row_ == 2});
    out.push_back({"SAVE LAST MAP ROAD", has_last ? "SAVE" : "NONE",
                   "Persists the last selected map road coordinates into settings.txt.", has_last, true, row_ == 3});
    out.push_back({"GPS TO SAVED ROAD", has_saved ? drive::format_road_point(*state.config.saved_map_road) : "NONE",
                   "Plots GPS sat-nav to your saved map road across restarts.", has_saved, true, row_ == 4});
    out.push_back({"TELEPORT TO SAVED ROAD", has_saved ? "TELEPORT" : "NONE",
                   "Open map and confirm to teleport to your saved road (10 s expiry).", has_saved, true, row_ == 5});
    return out;
  }

  out.push_back({"HIDDEN CARS (AUTOSHOW)", state.hidden_cars ? "UNLOCKED" : "OFF",
                 "Lists Intro Corrado + Showcase Mini, 500 Abarth & 205 T16 in Autoshow (traffic excluded).", true,
                 false, row_ == 0});
  out.push_back({"BLOOM", state.config.debug_bloom ? "ON" : "OFF",
                 "Live toggle for HDR bloom post-processing pass.", true, false, row_ == 1});
  out.push_back({"MOTION BLUR", state.config.debug_motion_blur ? "ON" : "OFF",
                 "Live toggle for radial/velocity motion blur.", true, false, row_ == 2});
  out.push_back({"DEPTH OF FIELD", state.config.debug_dof ? "ON" : "OFF",
                 "Live toggle for camera depth-of-field blur.", true, false, row_ == 3});
  out.push_back({"RECORD FFB TRACE CSV", state.config.debug_ffb_trace ? "RECORDING" : "OFF",
                 "Writes per-frame wheel FFB forces, steering and pedals to ffb-trace-<time>.csv.", true, false,
                 row_ == 4});
  out.push_back({"TELEMETRY PROBE CSV", state.config.debug_telemetry_probe ? "RECORDING" : "OFF",
                 "Writes car physics telemetry diagnostics to telemetry-probe.csv.", true, false, row_ == 5});
  return out;
}

void draw_trainer_ui4(ui::List& l, float screen_w, float screen_h, const TrainerOverlay& overlay,
                      const drive::TrainerState& state, int64_t credits, int wristband_level,
                      const std::string& save_tree_label) {
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.32f);

  ui4::title_banner(l, s, 60.0f * s, 26.0f * s, "F10 TRAINER",
                    std::string("ISOLATED SAVE: ") + save_tree_label + "  —  MAIN SAVE UNTOUCHED", true, 640.0f);

  const std::vector<std::string> tabs = {"PLAYER", "WORLD", "VEHICLE", "DEBUG & MODS"};
  const int cur_tab = int(overlay.active_tab());
  ui4::tab_strip(l, s, 56.0f * s, 114.0f * s, screen_w - 56.0f * s, tabs, cur_tab, true, ui4::kPink);

  const float top_y = 158.0f * s;
  const float bot_y = screen_h - 86.0f * s;
  const float lx0 = 56.0f * s;
  const float lx1 = screen_w * 0.62f;
  const float rx0 = lx1 + 18.0f * s;
  const float rx1 = screen_w - 56.0f * s;

  // Left card: trainer actions.
  ui4::panel(l, lx0, top_y, lx1, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, lx0 + 12.0f * s, top_y + 10.0f * s, lx1 - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(lx0 + 22.0f * s, top_y + 15.0f * s,
         tabs[size_t(cur_tab)] + (state.config.cheats_enabled ? "  —  CHEATS ACTIVE" : "  —  CHEATS OFF"),
         ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

  const auto rows = overlay.rows(state);
  const float row_h = 56.0f * s;
  float ry = top_y + 46.0f * s;
  const TrainerOverlay::RowView* sel_row = nullptr;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    if (r.selected) sel_row = &r;
    if (r.selected) {
      ui4::brush(l, lx0 + 8.0f * s, ry + 2.0f * s, lx1 - 8.0f * s, ry + row_h - 4.0f * s,
                 r.is_action ? ui4::kAmber : ui4::kPink);
    } else {
      l.rect(lx0 + 12.0f * s, ry + 2.0f * s, lx1 - 12.0f * s, ry + row_h - 4.0f * s,
             (i % 2 == 0) ? ui4::kRowAlt : ui::rgba(28, 30, 36, 196));
    }

    const uint32_t label_col = !r.enabled ? ui4::kDim : (r.selected && r.is_action ? ui4::kInk : ui4::kWhite);
    ui::TextStyle ns = ui4::style(r.selected ? ui::Font::kE : ui::Font::kB, 14.5f * s, label_col);
    ui4::fit_text(r.label, ns, (lx1 - lx0) - 230.0f * s, 10.0f * s);
    l.text(lx0 + 24.0f * s, ry + 18.0f * s, r.label, ns);

    const float vx0 = lx1 - 196.0f * s;
    const float vx1 = lx1 - 22.0f * s;
    l.rect(vx0, ry + 10.0f * s, vx1, ry + row_h - 14.0f * s, ui::rgba(14, 16, 20, 190));
    const std::string vstr = (r.selected && !r.is_action) ? ("< " + r.value + " >") : r.value;
    ui::TextStyle vs = ui4::style(ui::Font::kE, 13.5f * s,
                                  !r.enabled ? ui4::kDim : (r.selected ? ui4::kYellow : ui4::kWhite),
                                  ui::Align::kCenter);
    ui4::fit_text(vstr, vs, (vx1 - vx0) - 10.0f * s, 9.0f * s);
    l.text(0.5f * (vx0 + vx1), ry + 18.0f * s, vstr, vs);
    ry += row_h;
  }

  // Right card: isolated profile & active modifiers status.
  ui4::panel(l, rx0, top_y, rx1, bot_y, s, ui4::kCharcoalDark, ui4::kAmber, 0, true);
  ui4::offwhite_strip(l, rx0 + 12.0f * s, top_y + 10.0f * s, rx1 - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "SANDBOX PROFILE STATUS",
         ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

  if (sel_row) {
    ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s, ui4::kPink);
    ui::TextStyle stt = ui4::style(ui::Font::kE, 15.5f * s, ui4::kWhite);
    ui4::fit_text(sel_row->label, stt, (rx1 - rx0) - 34.0f * s, 10.5f * s);
    l.text(rx0 + 20.0f * s, top_y + 54.0f * s, sel_row->label, stt);

    ui::TextStyle ds = ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite, ui::Align::kLeft, true);
    ui4::fit_text(sel_row->detail, ds, (rx1 - rx0) - 40.0f * s, 9.5f * s);
    l.text(rx0 + 20.0f * s, top_y + 98.0f * s, sel_row->detail, ds);
  }

  float sy = top_y + 154.0f * s;
  auto info_box = [&](const char* hdr, const std::string& l1, const std::string& l2, uint32_t accent) {
    l.rect(rx0 + 14.0f * s, sy, rx1 - 14.0f * s, sy + 72.0f * s, ui::rgba(255, 255, 255, 14));
    l.rect(rx0 + 14.0f * s, sy, rx0 + 18.0f * s, sy + 72.0f * s, accent);
    l.text(rx0 + 28.0f * s, sy + 8.0f * s, hdr, ui4::style(ui::Font::kE, 11.5f * s, accent));
    l.text(rx0 + 28.0f * s, sy + 27.0f * s, l1, ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite));
    l.text(rx0 + 28.0f * s, sy + 46.0f * s, l2, ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    sy += 82.0f * s;
  };

  info_box("PLAYER (MODDED PROFILE)",
           "CREDITS: " + ui4::format_credits(credits),
           std::string("WRISTBAND: ") + drive::wristband_tier_name(wristband_level) + "  |  SAVE: " + save_tree_label,
           ui4::kAmber);

  info_box("WORLD & EVENTS",
           std::string("SPEED: ") + fmt_mult(state.effective_game_speed()) + "  |  TOD: " +
               fmt_tod(state.config.time_of_day_hour) + "  |  DAY: " + fmt_day_length(state.config.day_length_minutes),
           std::string("ANY CAR: ") + (state.effective_any_car() ? "ON" : "OFF") + "  |  FREE CAM: " +
               (state.effective_free_camera() ? "ON" : "OFF") + "  |  SIGNS: " +
               (state.effective_reveal_collectibles() ? "ON" : "OFF"),
           ui4::kPink);

  info_box("PACING & HIDDEN CARS",
           std::string("PRICES: ") + fmt_mult(state.pacing.effective_price_multiplier()) + "  |  REWARDS: " +
               fmt_mult(state.pacing.effective_reward_multiplier()),
           std::string("HIDDEN CARS: ") + (state.hidden_cars ? "4 VARIANTS UNLOCKED" : "STOCK CATALOG"),
           state.hidden_cars || state.pacing.enabled ? ui4::kGreen : ui4::kDim);

  if (!overlay.last_message().empty()) {
    ui4::pill(l, rx0 + 14.0f * s, bot_y - 42.0f * s, rx1 - 14.0f * s, bot_y - 12.0f * s, ui4::kGreen);
    ui::TextStyle ms = ui4::style(ui::Font::kE, 12.0f * s, ui4::kInk);
    ui4::fit_text(overlay.last_message(), ms, (rx1 - rx0) - 48.0f * s, 9.0f * s);
    l.text(rx0 + 26.0f * s, bot_y - 34.0f * s, overlay.last_message(), ms);
  }

  ui4::button_bar(l,
                  {{"DPAD_LR", "CHANGE"},
                   {"LB", "TAB"},
                   {"A", "APPLY"},
                   {"B", "CLOSE (F10)"}},
                  screen_w, screen_h, 56.0f);
}

}  // namespace carview
