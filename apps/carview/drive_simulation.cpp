#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "hud_screen.h"
#include "race/rally_dialogue.h"
#include "world/road_follow.h"

namespace carview {

void step_ftc_scripted_cars(DriveSession& s, double dt);

DriveSession::Popup DriveSession::Popup::notify(NotifyKind kind, double at, const std::vector<std::string>& title_args,
                                                const std::vector<std::string>& message_args,
                                                const std::vector<std::string>& tag_args) {
  const NotifyText t = notify_text(kind, tag_args, title_args, message_args);
  Popup p{t.title, t.message, at, -1, ui::rgba(245, 4, 92)};
  p.kind = kind;
  p.tag = t.tag;
  if (const NotifyStrings* e = notify_strings(kind)) p.icon = e->icon;
  return p;
}

namespace {

// A sponsor challenge's tier reached: the original's sponsor notification,
// RANK <tier> UNLOCKED! over the reward and the challenge's name, with the
// sponsor's logo.
DriveSession::Popup sponsor_popup(DriveSession& s, std::string_view challenge_id, int tier, int64_t credits) {
  auto p = DriveSession::Popup::notify(NotifyKind::kSponsorRank, s.vehicle.time(), {"+" + thousands(credits)}, {},
                                       {std::to_string(tier)});
  const auto rows = s.sponsor_rows();
  const auto& defs = s.app.skills_data.sponsor_challenges;
  for (size_t i = 0; i < rows.size() && i < defs.size(); ++i)
    if (defs[i].id == challenge_id) {
      p.text = rows[i].challenge_title;
      p.logo = rows[i].logo_texture;
      break;
    }
  if (p.text.empty()) p.text = std::string(challenge_id);
  return p;
}

// PROG-11: steps the free-roam (co-op) challenges on this step's distance,
// speed, time of day, car, skill awards and speed camera results; pays and
// announces each one completed.
void step_freeroam_challenges(DriveSession& s) {
  auto& app = s.app;
  freeroam::ChallengeStepInput in;
  in.time_s = s.vehicle.time();
  const drive::Vec3 v = s.vehicle.velocity();
  const double speed = std::max(std::fabs(s.vehicle.speed()), std::hypot(v.x, v.z));
  in.distance_m = speed * drive::Vehicle::kStep;
  in.speed_mph = speed * 2.2369362920544;
  in.tod_hour = app.tod_clock.hour();
  if (app.have_career_catalog)
    if (const auto* spec = app.career_catalog.find_car_by_media(app.car)) {
      // The catalog's 0 is "not set": left unknown so a restriction on it fails.
      // interim: PI and class are the stock values; upgrades are not reported.
      using F = freeroam::CarField;
      auto put = [&](F f, double v) {
        if (v > 0) in.car[f] = v;
      };
      in.car[F::kClass] = spec->stock_class;
      put(F::kPi, spec->stock_pi);
      put(F::kYear, spec->props.year);
      put(F::kBodyFamily, spec->props.body_family);
      put(F::kRegion, spec->props.region);
      put(F::kCountry, spec->props.country);
      put(F::kMake, spec->props.make);
      put(F::kModelFamily, spec->props.model_family);
      put(F::kDriveType, spec->props.drive_type);
      put(F::kEnginePlacement, spec->engine_placement);
      put(F::kBaseCost, spec->base_cost);
    }
  for (const auto& sk : s.club_pending.skills) in.skills.push_back(sk.type);
  for (const auto& cam : s.club_pending.cameras) in.flashes.push_back({cam.name, cam.speed_mph, 0});
  for (const auto& done : app.freeroam_challenges->step(in)) {
    const int64_t cr = s.slice12.trainer.pacing.scale_credits(done.credits);
    app.career.credits = std::min<int64_t>(999999999, app.career.credits + cr);
    app.career.total_winnings += cr;
    // interim: the sponsor notification, its tag the English of
    // FreeRoamChallenges.str's "CO-OP CHALLENGE" and "COMPLETED" lines.
    s.popup = DriveSession::Popup::notify(NotifyKind::kSponsorRank, s.vehicle.time(), {"+" + thousands(cr)});
    s.popup.tag = "CO-OP CHALLENGE COMPLETED";
    s.popup.text = done.def && !done.def->challenge_name.empty() ? done.def->challenge_name : done.id;
    if (done.def) s.popup.logo = done.def->logo_texture();
    std::printf("free-roam challenge %s completed: +%lld CR\n", done.id.c_str(), (long long)cr);
    s.save_due = true;
  }
}

// The popularity challenge's tier for a fame reward at `rank_required`.
int fame_tier(const DriveSession& s, int rank_required) {
  const auto& tiers = s.app.skills_data.fame_tiers;
  for (size_t i = 0; i < tiers.size(); ++i)
    if (tiers[i].rank_required == rank_required) return int(i + 1);
  return 1;
}

audio::SmashMaterial classify_smash_material(const std::string& stem_l, const std::string& stype,
                                             uint8_t& out_surface_hint) {
  auto has = [&](const char* needle) { return stem_l.find(needle) != std::string::npos; };
  if (stype == "fence" || has("fence") || has("gate") || has("barricade")) {
    out_surface_hint = (has("chain") || has("metal") || has("steel") || has("iron")) ? 3 : 4;
    return audio::SmashMaterial::kFence;
  }
  if (has("glass") || has("window") || has("bottle") || has("busstop") || has("lantern")) {
    out_surface_hint = 0;
    return audio::SmashMaterial::kGlass;
  }
  if (stype == "flower" || has("bush") || has("shrub") || has("tree") || has("cactus") || has("flower") ||
      has("plant") || has("hedge") || has("leaf") || has("foliage")) {
    out_surface_hint = 2;
    return audio::SmashMaterial::kFoliage;
  }
  if (has("stone") || has("rock") || has("brick") || has("concrete") || has("cinder") || has("planter") ||
      has("wall")) {
    out_surface_hint = 0;
    return audio::SmashMaterial::kStone;
  }
  if (stype == "cone" || stype == "bin" || has("plastic") || has("cone") || has("pylon") || has("delineator") ||
      has("trash") || has("bin") || has("garbage") || has("cooler") || has("drum") || has("chair")) {
    out_surface_hint = 0;
    return audio::SmashMaterial::kPlastic;
  }
  if (has("wood") || has("log") || has("lumber") || has("fruit") || has("crate") || has("plank") ||
      has("picnic") || has("bench") || has("barrel") || has("pallet") || has("barn") || has("hay") ||
      stype == "picnic") {
    out_surface_hint = 4;
    return audio::SmashMaterial::kWood;
  }
  if (stype == "sign" || stype == "pole" || stype == "camera" || has("metal") || has("hydrant") ||
      has("mailbox") || has("postbox") || has("pole") || has("sign") || has("camera") || has("meter") ||
      has("rail") || has("steel")) {
    out_surface_hint = 3;
    return audio::SmashMaterial::kMetal;
  }
  out_surface_hint = 0;
  return audio::SmashMaterial::kDefault;
}

struct RolloverRecoveryState {
  const DriveSession* owner = nullptr;
  double last_sim_time = -1.0;
  double rolled_s = 0.0;
};

RolloverRecoveryState g_rollover_state;

struct RewindGuardState {
  const DriveSession* owner = nullptr;
  double last_sim_time = -1.0;
};

RewindGuardState g_rewind_guard;

struct ViperConvoyState {
  const DriveSession* owner = nullptr;
  double last_spawn_t = -10.0;
  std::map<uint32_t, double> prev_ahead_m;
  std::set<uint32_t> counted_ids;
};

ViperConvoyState g_viper_convoy;

// A uniform [0, 1) draw for a RadioDJRequests delay window.
double radio_jitter() {
  static std::mt19937 rng{std::random_device{}()};
  return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
}

void apply_popularity_with_events(DriveSession& s, double pts) {
  if (!s.app.skill_tracker || pts <= 0.0) return;
  int prev_pop_rank = s.app.skill_tracker->popularity_rank();
  const auto evs = s.app.skill_tracker->add_popularity(pts, s.vehicle.time());
  for (const auto& sev : evs) {
    if (sev.kind == freeroam::SkillEvent::kFameSponsor && sev.credits > 0) {
      const int64_t scr = s.slice12.trainer.pacing.scale_credits(sev.credits);
      s.app.career.credits = std::min<int64_t>(999999999, s.app.career.credits + scr);
      s.app.career.total_winnings += scr;
      s.popup = sponsor_popup(s, sev.skill_id, fame_tier(s, sev.rank), scr);
      s.save_due = true;
    } else if (sev.kind == freeroam::SkillEvent::kRankUp) {
      const int old_rank = std::max(prev_pop_rank, sev.rank + 1);
      s.app.audio_host.trigger_ui(audio::UiCue::kRankUp);
      // Fame.xml radioFestivalUpdate of the level crossed (FestivalUpdateSkillLevel<rank>).
      if (!sev.radio_festival_update.empty()) s.app.audio_host.radio_trigger_festival_update(sev.radio_festival_update);
      s.app.campaign.on_popularity_rank_changed(old_rank, sev.rank, &s.app.audio_host);
      if (s.app.have_career_catalog) {
        for (const auto& cev : s.app.career_catalog.events) {
          if (cev.style == 3 && cev.popularity_rank_req > 0 &&
              sev.rank <= cev.popularity_rank_req && cev.popularity_rank_req < old_rank) {
            s.popup = DriveSession::Popup::notify(NotifyKind::kShowcaseUnlocked, s.vehicle.time());
          }
        }
      }
      prev_pop_rank = std::min(prev_pop_rank, sev.rank);
      s.save_due = true;
    }
  }
}

}  // namespace

audio::EnginePlayer::Update DriveSession::sound_inputs(audio::View audio_view) {
        audio::EnginePlayer::Update u;
        const double rpm = vehicle.rpm();
        // The throttle after the rev limiter's cut, as the original's car-audio
        // input reads the engine's throttle (Q341).
        double thr = vehicle.applied_throttle();
        // Retail non-manual-clutch shifts scale engine audio throttle by 0.75x.
        if (vehicle.shifting() && vehicle.assists().shifting != 2) {
          thr = std::max(thr, vehicle.shaped_input().throttle) * 0.75;
        }
        const double torque = vehicle.engine_torque(rpm, thr);
        const double full = vehicle.engine_torque(rpm, 1), closed = vehicle.engine_torque(rpm, 0);
        u.rpm = rpm;
        u.throttle = thr;
        u.pos_torque = torque > 0 && full > 0 ? torque / full : 0;
        u.neg_torque = torque < 0 && closed < 0 ? torque / closed : 0;
        // At idle the engine holds its rpm and is not braking: the original's
        // idle shows no NegLoad boost (Q322), so negative load fades in only
        // above 1.15x to 1.5x the idle rpm.
        const double idle = std::max(1.0, vehicle.params().idle_rpm);
        u.neg_torque *= std::clamp((rpm - 1.15 * idle) / (0.35 * idle), 0.0, 1.0);
        u.view = audio_view;
        return u;
}

void DriveSession::fire_satnav_event(const traffic::GpsPromptEvent& gev) {
        using K = traffic::GpsPromptEvent::Kind;
        using T = traffic::GpsTurnKind;
        using D = traffic::GpsDestKind;
        if (gev.kind == K::kNone) return;
        audio::SatNavCue cue = audio::SatNavCue::kDestGeneric;
        bool play = true;
        const bool is_roundabout_exit =
            (gev.turn == T::kRoundabout && gev.roundabout_exit >= 1 && gev.roundabout_exit <= 4);
        auto roundabout_cue = [](int exit_num) {
          switch (exit_num) {
            case 1: return audio::SatNavCue::kRoundaboutExit1;
            case 2: return audio::SatNavCue::kRoundaboutExit2;
            case 3: return audio::SatNavCue::kRoundaboutExit3;
            default: return audio::SatNavCue::kRoundaboutExit4;
          }
        };
        const T eff_turn = (gev.turn == T::kRoundabout)
                               ? (gev.exit_turn != T::kNone ? gev.exit_turn : T::kBearRight)
                               : gev.turn;
        switch (gev.kind) {
          case K::kDestinationSet:
            switch (gev.dest_kind) {
              case D::kFestival: cue = audio::SatNavCue::kDestFestival; break;
              case D::kFestivalRace: cue = audio::SatNavCue::kDestFestivalRace; break;
              case D::kShowcase: cue = audio::SatNavCue::kDestShowcase; break;
              case D::kOutpost: cue = audio::SatNavCue::kDestOutpost; break;
              case D::kWorkshop: cue = audio::SatNavCue::kDestWorkshop; break;
              case D::kAutoshow: cue = audio::SatNavCue::kDestAutoshow; break;
              case D::kPaintShop: cue = audio::SatNavCue::kDestPaintShop; break;
              case D::kRaceCentral: cue = audio::SatNavCue::kDestRaceCentral; break;
              case D::kStreetRace: cue = audio::SatNavCue::kDestStreetRace; break;
              default: cue = audio::SatNavCue::kDestGeneric; break;
            }
            break;
          case K::kDestinationCannotBeSet: cue = audio::SatNavCue::kDestCannotBeSet; break;
          case K::kRecalculating: cue = audio::SatNavCue::kRecalculating; break;
          case K::kUTurn: cue = audio::SatNavCue::kUTurn; break;
          case K::kArrivedExtended: cue = audio::SatNavCue::kExtendedDestination; break;
          case K::kArrivedFar: cue = audio::SatNavCue::kFarDestination; break;
          case K::kArrivedNear: cue = audio::SatNavCue::kNearDestination; break;
          case K::kArrived:
            cue = audio::SatNavCue::kArrived;
            if (!app.race && !app.campaign.ftc_active()) {
              popup = {"DESTINATION REACHED", "YOU HAVE ARRIVED AT YOUR WAYPOINT", vehicle.time(), -1,
                       carview::ui::rgba(74, 238, 97)};
            }
            break;
          case K::kTurnNow:
            if (is_roundabout_exit) {
              cue = roundabout_cue(gev.roundabout_exit);
              break;
            }
            switch (eff_turn) {
              case T::kLeft: cue = audio::SatNavCue::kTurnLeft; break;
              case T::kRight: cue = audio::SatNavCue::kTurnRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kSharpRight; break;
              case T::kUTurn: cue = audio::SatNavCue::kUTurn; break;
              default: play = false; break;
            }
            break;
          case K::kTurnNextJunction:
            if (is_roundabout_exit) {
              cue = roundabout_cue(gev.roundabout_exit);
              break;
            }
            switch (eff_turn) {
              case T::kLeft: cue = audio::SatNavCue::kNextJuncLeft; break;
              case T::kRight: cue = audio::SatNavCue::kNextJuncRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kNextJuncBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kNextJuncBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kNextJuncSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kNextJuncSharpRight; break;
              case T::kUTurn: cue = audio::SatNavCue::kUTurn; break;
              default: play = false; break;
            }
            break;
          case K::kTurnNear:
            if (is_roundabout_exit) {
              cue = roundabout_cue(gev.roundabout_exit);
              break;
            }
            switch (eff_turn) {
              case T::kLeft: cue = audio::SatNavCue::kNearLeft; break;
              case T::kRight: cue = audio::SatNavCue::kNearRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kNearBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kNearBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kNearSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kNearSharpRight; break;
              case T::kUTurn: cue = audio::SatNavCue::kUTurn; break;
              default: play = false; break;
            }
            break;
          case K::kTurnFar:
            if (is_roundabout_exit) {
              cue = roundabout_cue(gev.roundabout_exit);
              break;
            }
            switch (eff_turn) {
              case T::kLeft: cue = audio::SatNavCue::kFarLeft; break;
              case T::kRight: cue = audio::SatNavCue::kFarRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kFarBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kFarBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kFarSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kFarSharpRight; break;
              case T::kUTurn: cue = audio::SatNavCue::kUTurn; break;
              default: play = false; break;
            }
            break;
          case K::kTurnExtended:
            if (is_roundabout_exit) {
              cue = roundabout_cue(gev.roundabout_exit);
              break;
            }
            switch (eff_turn) {
              case T::kLeft: cue = audio::SatNavCue::kExtendedLeft; break;
              case T::kRight: cue = audio::SatNavCue::kExtendedRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kExtendedBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kExtendedBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kExtendedSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kExtendedSharpRight; break;
              case T::kUTurn: cue = audio::SatNavCue::kUTurn; break;
              default: play = false; break;
            }
            break;
          default: play = false; break;
        }
        if (play) app.audio_host.trigger_satnav(cue);
}

void DriveSession::start_pr_stunt(const freeroam::PrStunt& st) {
        int64_t base_pts = 0;
        if (app.skill_tracker) {
          const auto sh = app.skill_tracker->hud();
          base_pts = int64_t(std::llround(sh.popularity_points));
        }
        app.pr_stunt_session.start(st, base_pts, int(app.race_options.difficulty));
        pr_stunt_has_prev = false;
        pr_stunt_photo_pressed = false;
        // Swap to the PR Stunt's required loaner car when specified (storing pre_loaner_car to restore after).
        if (st.loaner_car_id > 0) {
          if (auto it = app.baked_ids.find(st.loaner_car_id);
              it != app.baked_ids.end() && lower_ascii(it->second) != lower_ascii(app.car)) {
            if (app.pre_loaner_car.empty()) {
              app.pre_loaner_car = app.car;
              app.pre_loaner_garage_id = app.career_garage.selected;
            }
            app.pr_stunt_loaner_active = true;
            const drive::Vec3 curp = vehicle.position();
            const drive::Vec3 curf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            app.spawn[0] = curp.x;
            app.spawn[1] = curp.y;
            app.spawn[2] = curp.z;
            app.spawn[3] = std::atan2(curf.x, curf.z);
            app.have_spawn = true;
            app.next_car = it->second;
            running = false;
          }
        }
        const drive::Vec3 at = vehicle.position();
        if (app.gps_solver.ready()) {
          fire_satnav_event(app.gps_nav.set_destination(st.target_pos.x, st.target_pos.z, traffic::GpsDestKind::kGeneric,
                                                    st.title, at.x, at.z));
        }
        popup = {freeroam::pr_stunt_kind_name(st.kind), st.title, vehicle.time(), -1,
                 carview::ui::rgba(255, 146, 3)};
        app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
}

bool DriveSession::free_roam_step() {
        const drive::Vec3 pos_now = vehicle.position();
        // Gate road discovery during closed races and first-time career so
        // ROADS DRIVEN toasts never leak over FTC drives or races.
        if (app.road_discovery.ready() && !app.race && !app.campaign.ftc_active()) {
          for (const auto& rev : app.road_discovery.update(pos_now.x, pos_now.z)) {
            char rb[96];
            std::snprintf(rb, sizeof rb, "ROAD DISCOVERED  %d / %d", rev.discovered_count, rev.total_count);
            std::string rname = rev.road_name;
            for (char& ch : rname) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            if (rname.rfind("ROAD_", 0) == 0 && rname.size() > 5) {
              std::string num = rname.substr(5);
              while (num.size() > 1 && num.front() == '0') num.erase(num.begin());
              rname = "COLORADO ROAD #" + num;
            } else {
              for (char& ch : rname) {
                if (ch == '_') ch = ' ';
              }
            }
            popup = Popup::notify(NotifyKind::kRoadsDriven, vehicle.time(), {},
                                  {std::to_string(rev.discovered_count), std::to_string(rev.total_count)});
            std::printf("t %.2f s: %s (%s)\n", vehicle.time(), rb, rname.c_str());
            app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
            save_due = true;
          }
        }
        if (!app.race) {
          const drive::Vec3 at = pos_now;
          auto restore_pr_stunt_loaner = [&]() {
            if (!app.pr_stunt_loaner_active) return;
            app.pr_stunt_loaner_active = false;
            if (!app.pre_loaner_car.empty()) {
              const std::string back_car = app.pre_loaner_car;
              if (app.pre_loaner_garage_id > 0) app.career_garage.selected = app.pre_loaner_garage_id;
              app.pre_loaner_car.clear();
              app.pre_loaner_garage_id = 0;
              if (lower_ascii(back_car) != lower_ascii(app.car)) {
                const drive::Vec3 curp = vehicle.position();
                const drive::Vec3 curf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = curp.x;
                app.spawn[1] = curp.y;
                app.spawn[2] = curp.z;
                app.spawn[3] = std::atan2(curf.x, curf.z);
                app.have_spawn = true;
                app.next_car = back_car;
                app.car = back_car;
                running = false;
              }
            }
          };
          if (app.gps_nav.active()) {
            const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
            const double hdg = std::atan2(f.x, f.z);
            fire_satnav_event(app.gps_nav.update(at.x, at.z, hdg, std::fabs(vehicle.speed()), drive::Vehicle::kStep));
          }
          if (!app.colorado_world() && app.rally_catalog.loaded && !app.rally_hub_open &&
              notice_until <= vehicle.time()) {
            for (const auto& ch : app.rally_catalog.championships) {
              if (std::fabs(ch.trigger_x) < 1e-3 && std::fabs(ch.trigger_z) < 1e-3) continue;
              if (std::hypot(ch.trigger_x - at.x, ch.trigger_z - at.z) <= 55.0) {
                notice = ch.name + "  •  PRESS [E / ENTER] TO ENTER";
                notice_until = vehicle.time() + 1.5;
                break;
              }
            }
          }
          if (app.have_outposts) {
            if (!app.campaign.ftc_active()) {
              const int disc_idx = app.outpost_progress.check_discovery(app.outposts, at.x, at.z);
              if (disc_idx >= 0 && size_t(disc_idx) < app.outposts.outposts.size()) {
                const auto& op = app.outposts.outposts[size_t(disc_idx)];
                // Discovery only unlocks the fast-travel destination: no
                // credits or popularity in the original.
                const int dc = app.outpost_progress.discovered_count(app.outposts);
                popup = Popup::notify(NotifyKind::kOutpostDiscovered, vehicle.time());
                std::printf("t %.2f s: outpost %s discovered (%d/%zu)\n", vehicle.time(), op.display_name.c_str(), dc,
                            app.outposts.outposts.size());
                app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
                save_due = true;
              }
            }
            if (app.pr_stunt_session.active()) {
              freeroam::PrStuntUpdate psu;
              psu.car_x = at.x;
              psu.car_y = at.y;
              psu.car_z = at.z;
              psu.prev_x = pr_stunt_has_prev ? pr_stunt_prev_x : at.x;
              psu.prev_z = pr_stunt_has_prev ? pr_stunt_prev_z : at.z;
              psu.speed_mph = std::fabs(vehicle.speed()) * 2.2369363;
              if (app.skill_tracker) {
                const auto sh = app.skill_tracker->hud();
                psu.banked_skill_points = int64_t(std::llround(sh.popularity_points));
                psu.unbanked_skill_points = freeroam::banked_chain_points(sh.chain_points, sh.multiplier_f);
                psu.total_skill_points = psu.banked_skill_points;
              }
              if (have_cam_view) {
                psu.has_camera = true;
                psu.camera_pos = last_cam_view.eye;
                psu.camera_forward = last_cam_view.forward;
                psu.camera_up = last_cam_view.up;
                const auto& cf = last_cam_view.forward;
                const auto& cu = last_cam_view.up;
                double rx = cf[1] * cu[2] - cf[2] * cu[1];
                double ry = cf[2] * cu[0] - cf[0] * cu[2];
                double rz = cf[0] * cu[1] - cf[1] * cu[0];
                const double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
                if (rl > 1e-6) {
                  psu.camera_right = {rx / rl, ry / rl, rz / rl};
                }
                psu.camera_vfov_deg = last_cam_view.fov_degrees > 10.0 ? last_cam_view.fov_degrees : 55.0;
                psu.camera_aspect = double(std::max<uint32_t>(1, app.width)) / double(std::max<uint32_t>(1, app.height));
              }
              psu.photo_pressed = pr_stunt_photo_pressed;
              psu.dt = drive::Vehicle::kStep;
              pr_stunt_photo_pressed = false;
              pr_stunt_prev_x = at.x;
              pr_stunt_prev_z = at.z;
              pr_stunt_has_prev = true;
              const auto st_state = app.pr_stunt_session.update(psu);
              if (st_state == freeroam::PrStuntState::kSucceeded) {
                const auto& st = app.pr_stunt_session.stunt();
                // A PR stunt pays nothing in the original (first time or
                // repeat): completing it raises the outpost's fast-travel
                // discount, which the success popup reports.
                const bool first_time = app.outpost_progress.complete_stunt(st.id);
                const NotifyKind done = st.kind == freeroam::PrStuntKind::kSpeed   ? NotifyKind::kSpeedStuntDone
                                        : st.kind == freeroam::PrStuntKind::kPhoto ? NotifyKind::kPhotoShootDone
                                                                                   : NotifyKind::kSkillStuntDone;
                // No credits: the popup reads FAST TRAVEL DISCOUNT EARNED
                // (GasStation strings) on a first completion.
                popup = Popup::notify(done, vehicle.time());
                if (first_time) popup.text = text_of(map_text::kCardEarned);
                std::printf("t %.2f s: %s done%s, fast travel %lld CR\n", vehicle.time(), st.title.c_str(),
                            first_time ? "" : " (repeat)", static_cast<long long>(outpost_travel_fee(st.outpost_id)));
                app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
                app.gps_nav.clear();
                save_due = true;
                restore_pr_stunt_loaner();
              } else if (st_state == freeroam::PrStuntState::kFailed) {
                const auto& st = app.pr_stunt_session.stunt();
                const NotifyKind failed = st.kind == freeroam::PrStuntKind::kSpeed   ? NotifyKind::kSpeedStuntFailed
                                          : st.kind == freeroam::PrStuntKind::kPhoto ? NotifyKind::kPhotoShootFailed
                                                                                     : NotifyKind::kSkillStuntFailed;
                popup = Popup::notify(failed, vehicle.time());
                app.gps_nav.clear();
                restore_pr_stunt_loaner();
              }
            } else if (app.pr_stunt_loaner_active) {
              // PR stunt was cancelled (e.g. via Escape); restore player's car immediately.
              restore_pr_stunt_loaner();
            }
          } else if (app.pr_stunt_loaner_active && !app.pr_stunt_session.active()) {
            restore_pr_stunt_loaner();
          }
        }
        // Recreate FreeRoam lazily if a new career reset cleared host.free_roam across a hot-reload.
        if (!host.free_roam && app.colorado_world() && !app.collectibles.signs.empty()) {
          host.free_roam = std::make_unique<freeroam::FreeRoam>(app.collectibles, app.progress);
          app.progress_loaded = true;
          host.hidden_sent = host.free_roam->hidden();
          host.world_gather_options.hidden = host.hidden_sent;
          if (host.streamer) host.streamer->set_hidden(host.hidden_sent);
          if (host.free_roam->hidden()) app.smashables.sync_barns_from_hidden(*host.free_roam->hidden());
        }
        if (!host.free_roam || app.campaign.flow().is_onboarding()) return false;
        freeroam::CarState cs;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
        const double fl = std::hypot(f.x, f.z);
        cs.x = at.x, cs.y = at.y, cs.z = at.z;
        if (fl > 1e-6) cs.forward_x = f.x / fl, cs.forward_z = f.z / fl;
        cs.half_width = vehicle.params().size.x / 2;
        cs.half_length = vehicle.params().size.z / 2;
        const drive::Vec3 v_lin = vehicle.velocity();
        const double planar_spd = std::hypot(v_lin.x, v_lin.z);
        const double fwd_spd = vehicle.speed();
        cs.speed = (std::fabs(fwd_spd) >= planar_spd) ? fwd_spd : (fwd_spd < 0.0 ? -planar_spd : planar_spd);
        cs.moved = std::max(std::fabs(fwd_spd), planar_spd) * drive::Vehicle::kStep;
        cs.dt = drive::Vehicle::kStep;
        // The barn controller's predicates: free roam, and no PR stunt or
        // speed zone running (activity_dependencies.xml blocks barn finds
        // during them). Dialogue is not tracked (interim, SPEC Q116).
        {
          const auto& bt = app.barn_tuning;  // GlobalRegistry Activities/BarnFinds
          host.free_roam->set_barn_tuning(bt.time_between_ticks_s, bt.max_idle_time_s, bt.min_distance_before_next_m,
                                          bt.min_speed_before_spawn_ms, bt.min_time_before_spawn_s);
        }
        // --barn-reveal: that barn's rumour reveal, once the world is up.
        if (!app.cli_barn_reveal.empty() && vehicle.time() > 3.0 && app.window && !map_open) {
          for (size_t bi = 0; bi < app.collectibles.barns.size(); ++bi) {
            if (app.collectibles.barns[bi].name != app.cli_barn_reveal) continue;
            // The rumour starts as the controller would (an unfound barn).
            if (bi >= app.progress.barns.size()) app.progress.barns.resize(app.collectibles.barns.size());
            auto& bp = app.progress.barns[bi];
            if (!freeroam::barn_discovered(bp.status) && bp.status != freeroam::kBarnRumour)
              bp.status = freeroam::kBarnRumour, bp.rumour_at = std::trunc(app.progress.total_distance);
            map_reveal_start(bi);
          }
          app.cli_barn_reveal.clear();
        }
        freeroam::BarnContext barn_ctx;
        barn_ctx.free_roam = app.race == nullptr;
        barn_ctx.blocked = app.pr_stunt_session.active() || (app.speed_tracker && app.speed_tracker->zone_active());
        bool sign_smashed_now = false;
        for (const auto& ev : host.free_roam->step(cs, barn_ctx)) {
          char line[160];
          if (ev.kind == freeroam::Event::kSignSmashed) {
            sign_smashed_now = true;
            const auto& sign_at = app.collectibles.signs[size_t(ev.index)].at;
            const std::array<float, 3> sign_pos{float(sign_at.x), float(sign_at.y), float(sign_at.z)};
            // Immediately hide the intact static billboard mesh on the GPU (persistent so
            // nearby smashable respawns / reset_broken() never unhide it) and persist across regathers.
            host.showroom.hide_world_props_near(sign_pos, 3.5f, true);
            app.smashables.record_hidden_prop(sign_pos, 3.5f);
            host.world_gather_options.hidden_positions = app.smashables.broken_hidden_positions();
            if (host.streamer) host.streamer->set_hidden_positions(host.world_gather_options.hidden_positions);
            app.audio_host.trigger_ui(audio::UiCue::kSignSmash);
            audio::CollisionHit bhit{};
            bhit.rel_speed_mps = float(std::fabs(cs.speed));
            bhit.pos = {sign_pos[0], sign_pos[1], sign_pos[2]};
            bhit.is_break = true;
            bhit.surface_hint = 3;
            bhit.smash_material = audio::SmashMaterial::kMetal;
            app.audio_host.trigger_collision(bhit);
            const drive::Vec3 vel_s = vehicle.velocity();
            host.particle_sys.spawn_smashable_burst("flyer", {sign_pos[0], sign_pos[1] + 0.8f, sign_pos[2]},
                                               {float(vel_s.x), float(vel_s.y + 2.5), float(vel_s.z)});
            app.smashables.spawn_flyer_shatter(
                sign_pos,
                {float(sign_at.x_axis[0]), float(sign_at.x_axis[1]), float(sign_at.x_axis[2])},
                {float(sign_at.y_axis[0]), float(sign_at.y_axis[1]), float(sign_at.y_axis[2])},
                {float(sign_at.z_axis[0]), float(sign_at.z_axis[1]), float(sign_at.z_axis[2])},
                {float(vel_s.x), float(vel_s.y), float(vel_s.z)});
            const int n = host.free_roam->signs_found();
            std::snprintf(line, sizeof line, "discount sign %d/%zu - upgrades %d percent off", n, app.collectibles.signs.size(),
                          freeroam::upgrade_discount(n));
            std::printf("t %.2f s: %s smashed (%s)\n", vehicle.time(), app.collectibles.signs[size_t(ev.index)].id.c_str(), line);
            popup = Popup::notify(NotifyKind::kDiscountSigns, vehicle.time(),
                                  {std::to_string(n), std::to_string(app.collectibles.signs.size())},
                                  {std::to_string(freeroam::upgrade_discount(n)) + "%"});
          } else if (ev.kind == freeroam::Event::kRumourStarted) {
            std::snprintf(line, sizeof line, "barn find rumour - M for the map");
            popup = Popup::notify(NotifyKind::kBarnRumour, vehicle.time());
            app.campaign.on_barn_find(false, app.collectibles.barns[size_t(ev.index)].name, "", &app.audio_host);
            // The barn's RadioDJRequests (barnfinds.xml): the location hint now, the
            // disabled "spotted" reminder inside its window while the rumour stands.
            for (const auto& rq : app.collectibles.barns[size_t(ev.index)].radio_requests) {
              const double d = rq.delay_min_s + (rq.delay_max_s - rq.delay_min_s) * radio_jitter();
              if (rq.immediate) app.audio_host.radio_trigger_immediate(rq.event);
              else app.audio_host.radio_schedule_special(rq.event, rq.enabled ? rq.delay_min_s : d);
            }
            std::printf("t %.2f s: %s rumour at %.1f km driven\n", vehicle.time(),
                        app.collectibles.barns[size_t(ev.index)].name.c_str(), app.progress.total_distance / 1000);
            // barnfindspawn_root: the paused map opens on the rumour (not in
            // headless or scripted runs, which nobody closes the map in).
            if (!app.headless && app.window && app.script_path.empty() && !app.race)
              map_reveal_start(size_t(ev.index));
          } else if (ev.kind == freeroam::Event::kBarnReady) {
            // Restored: BARN FIND READY - {car}, Dak's call, then the garage collects it.
            const auto& b = app.collectibles.barns[size_t(ev.index)];
            std::string named = barn_car_label(b);
            if (!b.car_name_key.empty()) {
              const uint32_t ref = uint32_t(fh1::string_hash("Barnfind")) << 16 | fh1::string_hash(b.car_name_key);
              const std::string t = carview::game_text("_&" + std::to_string(ref));
              if (!t.empty() && t[0] != '_') named = t;
            }
            // Ready is not yet in the garage: the "car added" sting plays
            // when the garage collects it.
            popup = Popup::notify(NotifyKind::kBarnReady, vehicle.time(), {named});
            apply_campaign_action(app.campaign.on_barn_restored(b.name, named, &app.audio_host));
            std::printf("t %.2f s: %s restored after %.1f km: %s ready at the garage\n", vehicle.time(),
                        b.name.c_str(), (app.progress.total_distance - app.progress.barns[size_t(ev.index)].found_at) / 1000,
                        named.c_str());
          } else {
            const auto& b = app.collectibles.barns[size_t(ev.index)];
            for (const auto& rq : b.radio_requests) app.audio_host.radio_cancel_scheduled(rq.event);
            if (!app.have_garage && app.have_career_catalog) {
              if (const auto* spec = app.career_catalog.find_car_by_media(app.car))
                app.award_garage_car(spec->car_id, 0, true);
            }
            // The car's name: the Barnfind table's IDS_ key, by the id rule.
            std::string named = barn_car_label(b);
            if (!b.car_name_key.empty()) {
              const uint32_t ref = uint32_t(fh1::string_hash("Barnfind")) << 16 | fh1::string_hash(b.car_name_key);
              const std::string t = carview::game_text("_&" + std::to_string(ref));
              if (!t.empty() && t[0] != '_') named = t;
            }
            // The title's flow: discovery starts the restoration (status 1);
            // the car is ready after a driven distance and is collected at
            // the garage.
            std::snprintf(line, sizeof line, "barn find! dak is restoring %s", named.c_str());
            popup = Popup::notify(NotifyKind::kBarnRestoring, vehicle.time(), {named});
            app.campaign.on_barn_find(true, b.name, named, &app.audio_host);
            std::printf("t %.2f s: %s found: %s (Data_Car %d), restored after %.1f km\n", vehicle.time(), b.name.c_str(),
                        named.c_str(), b.car_id,
                        freeroam::barn_restore_distance(b, app.progress.barns[size_t(ev.index)].found_at) / 1000);
          }
          save_due = true;
        }
        if (host.free_roam->hidden() != host.hidden_sent) {
          host.hidden_sent = host.free_roam->hidden();
          host.world_gather_options.hidden = host.hidden_sent;
          if (host.streamer) host.streamer->set_hidden(host.hidden_sent);
        }
        return sign_smashed_now;
}

int64_t DriveSession::current_make_id() {
        if (app.have_career_catalog) {
          if (const auto* spec = app.career_catalog.find_car_by_media(app.car)) return int64_t(spec->props.make);
        }
        return 0;
}

bool DriveSession::step(const drive::Input& user_in) {
        const auto ph_t0 = std::chrono::steady_clock::now();
        // R3-CAM-10 / R3-CAM-11: Detect rewind resume (sim time jumped backward to an earlier keyframe)
        // and immediately cancel any active unbanked Skill Chain and any active Speed Zone run.
        if (g_rewind_guard.owner != this) {
          g_rewind_guard.owner = this;
          g_rewind_guard.last_sim_time = vehicle.time();
        } else if (vehicle.time() < g_rewind_guard.last_sim_time - 0.05) {
          if (app.skill_tracker) app.skill_tracker->clear_active_chain();
          if (app.speed_tracker) {
            app.speed_tracker->cancel_active();
            app.audio_host.set_speed_zone_active(false);
            have_prev_speed_pos = false;
          }
        }
        g_rewind_guard.last_sim_time = vehicle.time();
        // Reset-to-Road UX (GameTunableSettings.ini [HandOfGodReset]): FadeOutTime
        // fade-out -> safe centerline reposition with zero velocity & camera
        // re-center -> FadeInTime fade-in; no car contact for CollisionsOffTime
        // into the fade-out and PostResetNonCollideTime after the reposition.
        auto begin_smooth_reset = [&](const drive::Vehicle::Pose& pre_pose, const drive::Vehicle::Pose& tgt_pose,
                                      std::string msg) {
          reset_ux.target_pose = tgt_pose;
          reset_ux.target_pose.vel = {};
          reset_ux.target_pose.ang = {};
          reset_ux.has_target_pose = true;
          reset_ux.phase = ResetToRoadState::Phase::kFadeOut;
          reset_ux.elapsed_s = 0.0;
          reset_ux.pending_notice = std::move(msg);
          vehicle.set_pose(pre_pose);
          drawn_before = pre_pose;
          rig.clear_pending_restart();
          if (!app.campaign.flow().transition().active) {
            // Fully covered at the reposition (FadeOutTime), uncovered over FadeInTime.
            app.campaign.flow().transition().start_split(race::TransitionKind::kCrossFade, reset_timing.fade_out,
                                                         reset_timing.fade_in);
          }
        };
        if (rig.restart_count() != reset_ux.seen_rig_restarts) {
          reset_ux.seen_rig_restarts = rig.restart_count();
          if (!app.headless && !app.bench && reset_ux.has_last_live_pose && vehicle.time() > 0.05 &&
              reset_ux.phase == ResetToRoadState::Phase::kIdle) {
            const drive::Vec3 cur_p = vehicle.position();
            const drive::Vec3 prev_p =
                reset_ux.last_live_pose.pos -
                drive::rotate(reset_ux.last_live_pose.rot, vehicle.params().centre_of_mass);
            if (std::hypot(cur_p.x - prev_p.x, cur_p.z - prev_p.z) < 200.0) {
              begin_smooth_reset(reset_ux.last_live_pose, vehicle.pose(), "");
            }
          }
        }
        if (reset_ux.ghost_remaining_s > 0.0) {
          reset_ux.ghost_remaining_s = std::max(0.0, reset_ux.ghost_remaining_s - drive::Vehicle::kStep);
        }
        bool hold_for_reset_fade = false;
        if (reset_ux.phase == ResetToRoadState::Phase::kFadeOut) {
          hold_for_reset_fade = true;
          reset_ux.elapsed_s += drive::Vehicle::kStep;
          // Calmly damp residual motion while fading out so the car does not keep tumbling.
          const double damp = std::exp(-8.0 * drive::Vehicle::kStep);
          const drive::Vec3 v_cur = vehicle.velocity();
          const drive::Vec3 a_cur = vehicle.angular_velocity();
          vehicle.set_velocity(v_cur * damp, a_cur * damp);
          if (reset_ux.elapsed_s >= reset_timing.fade_out) {
            if (reset_ux.has_target_pose) {
              vehicle.set_pose(reset_ux.target_pose);
              vehicle.set_velocity({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
              drawn_before = vehicle.pose();
            }
            camera_pull = CameraPull{};
            rig.restart();
            rig_accel.reset();
            reset_ux.seen_rig_restarts = rig.restart_count();
            rewind_buf.reset_history(vehicle.time());
            world_rewind_snaps.clear();
            reset_ux.has_target_pose = false;
            reset_ux.phase = ResetToRoadState::Phase::kFadeIn;
            reset_ux.elapsed_s = 0.0;
            reset_ux.ghost_remaining_s = reset_timing.post_reset_non_collide;
            if (!reset_ux.pending_notice.empty()) {
              notice = reset_ux.pending_notice;
              notice_until = vehicle.time() + 2.5;
              reset_ux.pending_notice.clear();
            }
          }
        } else if (reset_ux.phase == ResetToRoadState::Phase::kFadeIn) {
          reset_ux.elapsed_s += drive::Vehicle::kStep;
          if (reset_ux.elapsed_s >= reset_timing.fade_in) {
            reset_ux.phase = ResetToRoadState::Phase::kIdle;
            reset_ux.elapsed_s = 0.0;
          }
        }
        // Snap player vehicle onto live road surface before the first race physics step
        // so elevated route transform Y values never drop the car onto the starting grid.
        if (app.race && host.ground && vehicle.time() <= 1e-6 && !app.race->session().started()) {
          const drive::Vec3 vp = vehicle.position();
          if (std::isfinite(vp.x) && std::isfinite(vp.y) && std::isfinite(vp.z)) {
            drive::Hit ghit{};
            double road_y = vp.y;
            bool hit_ok = false;
            if (host.ground->raycast({vp.x, vp.y + 5.0, vp.z}, {0.0, -1.0, 0.0}, 50.0, ghit)) {
              road_y = (vp.y + 5.0) - ghit.distance;
              hit_ok = true;
            } else if (host.ground->raycast({vp.x, vp.y + 60.0, vp.z}, {0.0, -1.0, 0.0}, 120.0, ghit)) {
              road_y = (vp.y + 60.0) - ghit.distance;
              hit_ok = true;
            }
            if (hit_ok && std::fabs(vp.y - (road_y + 0.05)) > 0.35) {
              const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
              vehicle.reset({vp.x, road_y + 0.05, vp.z}, std::atan2(f.x, f.z));
              drawn_before = vehicle.pose();
            }
          }
        }
        // SPEC-CAR-PHYSICS, Aerodynamic drafting in free roam (behind traffic & festival AI).
        if (!app.race) {
          thread_local std::vector<drive::DraftCar> leaders;
          leaders.clear();
          if (traffic && traffic->cars()) {
            for (const auto& tc : *traffic->cars()) {
              if (tc.speed < 8.0) continue;
              leaders.push_back({{tc.position.x, tc.position.y, tc.position.z},
                                 {std::sin(tc.heading), 0.0, std::cos(tc.heading)},
                                 tc.speed,
                                 1.9,
                                 0.0,
                                 0.0});
            }
          }
          if (festival_mgr) {
            for (const auto& fc : festival_mgr->cars()) {
              if (fc.speed < 8.0) continue;
              leaders.push_back(
                  {{fc.position.x, fc.position.y, fc.position.z}, {std::sin(fc.heading), 0.0, std::cos(fc.heading)}, fc.speed, 1.9, 0.0, 0.0});
            }
          }
          drive::DraftCar me{};
          me.position = vehicle.position();
          me.forward = drive::rotate(vehicle.orientation(), {0, 0, 1});
          me.speed = vehicle.speed();
          me.width = vehicle.params().size.x > 0.5 ? vehicle.params().size.x : 1.9;
          me.front_df_coeff = vehicle.params().aero_df_front + vehicle.params().aero_part_df_front;
          me.rear_df_coeff = vehicle.params().aero_df_rear + vehicle.params().aero_part_df_rear;
          vehicle.set_draft(drive::compute_follower_draft(me, leaders.data(), leaders.size()));
        }
        drive::Input eff_user_in = user_in;
        // --autoplay's `drive auto`: the player's own input comes from the
        // sat-nav autopilot (a capture harness, not a game feature).
        if (app.autoplay.drive_auto() && !app.race) eff_user_in = ftc_autopilot_input();
        if (app.campaign.ftc_active()) {
          ftc_throttle = eff_user_in.throttle;
          ftc_handbrake = ftc_handbrake || eff_user_in.handbrake;
          // Controls off while the flow drives: the AI drives the player's
          // car on the route (the opening, the festival arrival), else the
          // car is held.
          if (!app.campaign.ftc().controls_enabled())
            eff_user_in = app.campaign.ftc().ai_driving() ? ftc_autopilot_input() : drive::Input{};
        } else if (app.campaign.tutor().active()) {
          ftc_throttle = eff_user_in.throttle;
          ftc_handbrake = ftc_handbrake || eff_user_in.handbrake;
        }
        if (hold_for_reset_fade) {
          eff_user_in = drive::Input{};
          eff_user_in.brake = 1.0;
        }
        // The rewards (results_page > 0): the car waits at the post-race
        // location, out of the AI's hands (PrepareForRewardsFlow). The race
        // still steps its field.
        drive::Input in = app.race ? app.race->player_input(vehicle, eff_user_in, traffic.get()) : eff_user_in;
        // Under --race-auto the race's autopilot is the player's hands: the
        // driving tutor hears its throttle, so PRESS RT TO ACCELERATE goes
        // as it would for a player who accelerates.
        if (app.race && app.race_options.autopilot && (app.campaign.ftc_active() || app.campaign.tutor().active()))
          ftc_throttle = std::max(ftc_throttle, in.throttle);
        // Handbrake and a light pedal: a held brake at a stop would select
        // reverse.
        if (app.race && results_page > 0) in = drive::Input{}, in.brake = 0.4, in.handbrake = true;
        if (!app.bench || app.bench_full_fx) {
          // Intact props and solid barriers meet the car's body (FEEL-21).
          const drive::Vec3 v0 = vehicle.velocity();
          const world::SmashableGround sm_ground(*host.ground, app.smashables, std::sqrt(drive::dot(v0, v0)));
          vehicle.step(in, sm_ground);
        } else {
          vehicle.step(in, *host.ground);
        }
        {
          // Thermal brake rotor integrator: heats proportionally to brake pedal * speed
          // above 3 m/s and cools exponentially with airflow (zero glow when parked).
          const float spd = float(std::fabs(vehicle.speed()));
          const float brk = float(std::clamp(in.brake, 0.0, 1.0));
          const float dt_s = float(drive::Vehicle::kStep);
          if (brk > 0.05f && spd > 3.0f) {
            const float heat_rate = brk * std::clamp((spd - 3.0f) / 36.0f, 0.0f, 1.8f) * 0.48f;
            rotor_heat = std::min(1.0f, rotor_heat + heat_rate * dt_s);
          }
          const float cool_tau = 5.5f / (1.0f + spd * 0.025f);
          rotor_heat *= std::exp(-dt_s / std::max(0.8f, cool_tau));
          if (rotor_heat < 0.002f) rotor_heat = 0.0f;
        }
        if (app.race) app.race->replay_player(vehicle);
        {
          // A car that left the numbers (NaN) stops the run: a gather around
          // NaN takes the whole map.
          const drive::Vec3 at = vehicle.position();
          if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
            std::fprintf(stderr, "carview: t %.3f s: the car's state is not finite (last at %.2f %.2f %.2f, %.1f km/h)\n",
                         vehicle.time(), last_finite.x, last_finite.y, last_finite.z, last_finite_kmh);
            app.error = "the car's state is not finite";
            return false;
          }
          last_finite = at;
          last_finite_kmh = vehicle.speed() * 3.6;
        }
        log_contacts();
        const auto ai_t0 = std::chrono::steady_clock::now();
        step_physics_ms += std::chrono::duration<double, std::milli>(ai_t0 - ph_t0).count();
        const bool reset_ghost_active = reset_ux.ghost_active(reset_timing.collisions_off);
        if (traffic) {
          const auto pre_traffic_pose = vehicle.pose();
          traffic->step(vehicle);
          if (reset_ghost_active && traffic->player_impulse() > 0.0) {
            // Post-reset no-contact window (PostResetNonCollideTime) keeps traffic
            // from immediately rear-ending the player right after resetting to the road.
            vehicle.set_pose(pre_traffic_pose);
          }
        }
        // Free-roam Festival AI Racers & instant Head-to-Head point-to-point race.
        // Stepped BEFORE SkillTracker so Festival AI collisions break the active skill chain
        // on the exact frame they occur and apply visual damage + realistic angular impulse.
        // Suppress random roaming Festival AI during the first-time career so generic
        // roaming drivers do not intrude into the Viper drive, Diner, or Race to Festival.
        if (!app.race && (app.campaign.ftc_active() || app.campaign.cutscenes().active())) {
          step_ftc_scripted_cars(*this, drive::Vehicle::kStep);
        }
        if (!app.race && festival_mgr && app.campaign.ftc_active()) {
          if (festival_mgr->h2h_active()) festival_mgr->cancel_head_to_head();
          if (!festival_mgr->cars().empty()) festival_mgr->restore_rewind({});
        } else if (!app.race && festival_mgr) {
          const drive::Vec3 fp = vehicle.position();
          const drive::Vec3 ff = drive::rotate(vehicle.orientation(), {0, 0, 1});
          const double fh = std::atan2(ff.x, ff.z);
          if (app.demo_h2h && !demo_h2h_started && vehicle.time() >= 3.0) {
            demo_h2h_started = true;
            const uint32_t fid = festival_mgr->spawn_ahead({fp.x, fp.y, fp.z}, fh, 18.0);
            if (fid && festival_mgr->start_head_to_head({fp.x, fp.y, fp.z}, fh, vehicle.time(), fid)) {
              club_pending.encounter_started = true;
              app.audio_host.trigger_ui(audio::UiCue::kH2hGo);
            }
            if (app.legacy_fm4_progression && app.have_prog_tables && app.career.driver_xp < 36000) {
              race::apply_progression_xp(app.career, 36000, current_make_id(), 18000, app.prog_tables);
            }
            app.sponsor_banner_title = "SPONSORSHIP TIER 2: NEAR MISS";
            app.sponsor_banner_detail = "+10,000 Cr  •  BONDURANT RACING SCHOOL";
            app.sponsor_banner_at = vehicle.time();
          }
          freeroam::HeadToHeadResult hres;
          festival_mgr->set_challenges_enabled(!app.campaign.ftc_active());
          if (festival_mgr->step(drive::Vehicle::kStep, vehicle.time(), {fp.x, fp.y, fp.z}, fh, vehicle.speed(),
                                 &hres, traffic ? traffic->cars() : nullptr)) {
            club_pending.encounter_completed = true;
            club_pending.encounter_finish_place = hres.won ? 1 : 2;
            app.audio_host.trigger_ui(hres.won ? audio::UiCue::kRaceFinish : audio::UiCue::kH2hLoss);
            ++app.h2h_races;
            if (hres.won) {
              ++app.h2h_wins;
              const int64_t hcr = slice12.trainer.pacing.scale_credits(hres.payout_credits);
              app.career.credits = std::min<int64_t>(999999999, app.career.credits + hcr);
              app.career.total_winnings += hcr;
            } else {
              // Retail FH1 does not deduct credits on Head-to-Head loss.
              popup = {"HEAD-TO-HEAD LOST",
                       "DEFEATED BY " + hres.opponent_name + "   +" + std::to_string(hres.popularity_points) + " FAME",
                       vehicle.time(), -1, carview::ui::rgba(255, 72, 72)};
            }
            apply_popularity_with_events(*this, double(hres.popularity_points));
            if (app.legacy_fm4_progression && app.have_prog_tables) {
              const int hxp = slice12.trainer.pacing.scale_points(hres.driver_xp);
              race::apply_progression_xp(app.career, hxp, current_make_id(), hxp / 2, app.prog_tables);
            }
            std::printf("h2h: %s vs %s (+%lld Cr, +%d Fame)\n", hres.won ? "WON" : "LOST",
                        hres.opponent_name.c_str(), (long long)hres.payout_credits, hres.popularity_points);
            save_due = true;
          }
          // Car-to-car physics collision between player and nearby Festival AI / H2H cars
          // (suppressed during the brief post-reset ghosting immunity window).
          if (!reset_ghost_active) {
            const drive::Vec3 fv = vehicle.velocity();
            const auto fcol = festival_mgr->resolve_player_collision(
                {fp.x, fp.y, fp.z}, {fv.x, fv.y, fv.z}, vehicle.params().mass, drive::Vehicle::kStep);
            if (fcol.hit && fcol.impulse_magnitude > 1.0) {
              constexpr double kFestivalAngularImpulseScale = 0.55;
              vehicle.apply_impulse({fcol.contact_point[0], fcol.contact_point[1], fcol.contact_point[2]},
                                    {fcol.player_impulse[0], fcol.player_impulse[1], fcol.player_impulse[2]},
                                    kFestivalAngularImpulseScale);
              step_car_impact_accum = std::max(step_car_impact_accum, fcol.impulse_magnitude);
              const double dv_f = fcol.impulse_magnitude / std::max(500.0, vehicle.params().mass);
              if (dv_f > 0.8) {
                audio::CollisionHit hit{};
                hit.rel_speed_mps = float(dv_f);
                hit.pos = {float(fcol.contact_point[0]), float(fcol.contact_point[1]), float(fcol.contact_point[2])};
                hit.is_car_on_car = true;
                hit.surface_hint = 1;
                app.audio_host.trigger_collision(hit);
                const drive::Vec3 rel{fcol.contact_point[0] - fp.x, fcol.contact_point[1] - fp.y,
                                      fcol.contact_point[2] - fp.z};
                const drive::Quat q = vehicle.orientation();
                const drive::Quat qinv{-q.x, -q.y, -q.z, q.w};
                const drive::Vec3 loc = drive::rotate(qinv, rel);
                car_damage.record_impact(float(loc.x), float(loc.y), float(loc.z), float(dv_f));
                if ((!app.bench || app.bench_full_fx) && dv_f > 2.5 && vehicle.time() - host.last_impact_vfx_time > 0.2) {
                  host.last_impact_vfx_time = vehicle.time();
                  const double ilen = std::max(1e-4, fcol.impulse_magnitude);
                  const std::array<float, 3> cpos{float(fcol.contact_point[0]), float(fcol.contact_point[1]),
                                                  float(fcol.contact_point[2])};
                  const std::array<float, 3> cnrm{float(fcol.player_impulse[0] / ilen), 0.15f,
                                                  float(fcol.player_impulse[2] / ilen)};
                  const std::array<float, 3> cvel{float(fv.x), float(fv.y), float(fv.z)};
                  host.particle_sys.spawn_collision_impact(cpos, cnrm, cvel, float(dv_f), true);
                }
              }
            }
          }
        }
        // R3-CAM-15: Viper opening drive Festival-bound AI convoy & pass countdown.
        if (!app.race && !app.campaign.ftc_active() &&
            app.campaign.flow().stage() == race::CampaignStage::kViperIntroDrive) {
          if (g_viper_convoy.owner != this || vehicle.time() < 0.05) {
            g_viper_convoy.owner = this;
            g_viper_convoy.last_spawn_t = -10.0;
            g_viper_convoy.prev_ahead_m.clear();
            g_viper_convoy.counted_ids.clear();
          }
          const drive::Vec3 fp = vehicle.position();
          const drive::Vec3 ff = drive::rotate(vehicle.orientation(), {0, 0, 1});
          const drive::Vec3 fr = drive::rotate(vehicle.orientation(), {1, 0, 0});
          const double fh = std::atan2(ff.x, ff.z);
          int convoy_ahead = 0;
          auto track_convoy_car = [&](uint32_t cid, double cx, double cz, double chdg) {
            const double dx = cx - fp.x, dz = cz - fp.z;
            const double ahead_m = dx * ff.x + dz * ff.z;
            const double lat_m = std::fabs(dx * fr.x + dz * fr.z);
            const double align = std::sin(chdg) * ff.x + std::cos(chdg) * ff.z;
            if (align > 0.45 && ahead_m > 10.0 && ahead_m < 280.0 && lat_m < 25.0) {
              ++convoy_ahead;
            }
            if (align > 0.45 && lat_m < 22.0) {
              auto it = g_viper_convoy.prev_ahead_m.find(cid);
              if (it != g_viper_convoy.prev_ahead_m.end() && it->second > 2.0 && ahead_m < -2.0) {
                if (g_viper_convoy.counted_ids.insert(cid).second) {
                  // Track ambient Festival-bound highway passes silently for radio banter
                  // (1:1 retail FH1 Viper intro objective stays strictly "DRIVE TO THE HORIZON FESTIVAL").
                  if (app.campaign.on_viper_convoy_pass(1, &app.audio_host)) {
                    app.story_intro.on_convoy_pass(1);
                  }
                }
              }
            }
            g_viper_convoy.prev_ahead_m[cid] = ahead_m;
          };
          if (festival_mgr) {
            for (const auto& fc : festival_mgr->cars()) {
              track_convoy_car(fc.id, fc.position.x, fc.position.z, fc.heading);
            }
            if (app.campaign.flow().viper_convoy_remaining() > 0 && convoy_ahead < 2 &&
                vehicle.speed() > 8.0 && vehicle.time() - g_viper_convoy.last_spawn_t >= 3.5) {
              const double dist_m = 75.0 + 35.0 * double(g_viper_convoy.counted_ids.size() % 3);
              if (festival_mgr->spawn_ahead({fp.x, fp.y, fp.z}, fh, dist_m) != 0) {
                g_viper_convoy.last_spawn_t = vehicle.time();
              }
            }
          }
          if (traffic && traffic->cars()) {
            for (const auto& tc : *traffic->cars()) {
              track_convoy_car(tc.id | 0x40000000u, tc.position.x, tc.position.z, tc.heading);
            }
          }
        }
        if (app.race && host.streamer) {
          app.race->after_step(vehicle, *host.ground, host.streamer->centre_x(), host.streamer->centre_z(),
                           host.streamer->collision_radius(), traffic.get());
          const bool r_started = app.race->session().started();
          if (r_started && !club_race_was_started) {
            club_pending.event_started = true;
            if (app.race->is_stage()) app.rally_stage_signals.start();
            prev_audio_checkpoint = app.race->player_checkpoints_passed();
            prev_audio_lap = int(app.race->session().car(app.race->player_index()).laps.size());
          }
          club_race_was_started = r_started;
          const auto& pc = app.race->session().car(app.race->player_index());
          const bool r_finished = (pc.finish >= 0.0);
          if (r_started) {
            const int cur_cp = app.race->player_checkpoints_passed();
            if (cur_cp > prev_audio_checkpoint && !r_finished) {
              app.audio_host.trigger_ui(audio::UiCue::kCheckpoint);
            }
            prev_audio_checkpoint = cur_cp;
            const int cur_lap = int(pc.laps.size());
            if (cur_lap > prev_audio_lap && !r_finished) {
              app.audio_host.trigger_ui(audio::UiCue::kLap);
            }
            prev_audio_lap = cur_lap;
          }
          if (r_finished && !club_race_was_finished) {
            club_pending.event_completed = true;
            club_pending.event_finish_place = app.race->player_place();
            app.audio_host.trigger_ui(audio::UiCue::kRaceFinish);
          }
          club_race_was_finished = r_finished;
          if (app.race->is_stage()) {
            const uint32_t cd_done = app.audio_host.telemetry().codriver_completed_cues;
            while (prev_codriver_completed < cd_done) {
              ++prev_codriver_completed;
              app.race->codriver().on_audio_cue_finished(app.race->session().clock());
            }
            race::CoDriverAudioCue cd_cue;
            while (app.race->codriver().pop_audio_cue(cd_cue)) {
              app.audio_host.trigger_codriver(cd_cue.group + "/" + cd_cue.cue);
            }
          }
        } else {
          club_race_was_started = false;
          club_race_was_finished = false;
        }
        if (app.race && app.race->is_stage() && app.race->player_finished() && app.rally_session.active() && !app.rally_service_open &&
            !app.rally_results_open) {
          const double ftime = app.race->stage_finish_time();
          const auto fin = app.rally_session.record_stage_finish(app.rally_catalog, ftime, app.race->stage_field());
          // Achievement 210: a stage win on Hard opponents, manual shifting, driving line off.
          if (fin.stage_place == 1 && app.race_options.difficulty >= race::Difficulty::kHard &&
              app.race_options.assists.shifting != 0 && app.race_options.assists.driving_line == 0)
            ++app.saved.ach.rally_hard_manual_stage_wins;
          // Achievement 214: a stage win without crashing or rewinding.
          if (app.rally_stage_signals.finish(fin.stage_place)) ++app.saved.ach.rally_flawless_stage_wins;
          // The co-driver's finish call (stages before the last; the heats'
          // own line), then Alice after the last stage (CPostRaceResults).
          {
            const auto line = race::rally_stage_finish_line(app.rally_session.current_stage_index() + 1,
                                                            app.rally_session.total_stages(), fin.stage_place,
                                                            app.rally_session.is_heats());
            if (!line.trigger.empty()) app.audio_host.trigger_vo(line.trigger, false, line.variant);
          }
          if (fin.championship_complete) {
            const int dpct = race::difficulty_bonus_percent(int(app.race_options.difficulty));
            const int apct = race::assist_difficulty_bonus_percent(app.race_options.assists);
            const auto rew = app.rally_session.finalize(app.rally_catalog, app.saved, dpct, apct);
            if (!rew.is_heats) {
              const auto* ch = app.rally_catalog.find_championship(rew.event_id);
              const auto line = race::rally_post_race_line(rew.final_place == 1,
                                                           ch && ch->series_id == race::kRallyFinalSeries,
                                                           app.saved.rally_first_win_vo);
              if (line.trigger == "PlayerWonFirstRally") app.saved.rally_first_win_vo = true;
              app.audio_host.trigger_vo(line.trigger, false, line.variant);
            }
            app.career.credits = app.saved.credits;
            app.career.total_winnings = app.saved.total_winnings;
            // R3-CAM-01: Merge finalized Rally Championship & Stage results into app.career.event_places
            // so save_now() never overwrites/wipes Rally completion records on the next save!
            for (const auto& [ev_id, place] : app.saved.event_results) {
              if (place <= 0) continue;
              auto it = app.career.event_places.find(ev_id);
              if (it == app.career.event_places.end() || it->second <= 0 || place < it->second) {
                app.career.event_places[ev_id] = place;
              }
            }
            app.rally_results_state = carview::make_rally_results_state(app.rally_catalog, app.rally_session, rew, app.saved.credits);
            // The champion sequence plays once (CShouldShowRallyEndSequence;
            // CSetHasSeenRallyEndSequence when it ends, rally_post_finish):
            // its screen, Alice's line and Rally_Champion_Cutscene come after
            // the points (rally_post_steps).
            app.rally_results_state.final_won = app.saved.rally_end_seen || rew.champion_unlocked;
            if (!rew.champion_unlocked || app.saved.rally_end_seen) app.rally_results_state.champion_unlocked = false;
            app.rally_results_open = true;
            rally_post_enter_step();
            save_due = true;
          } else {
            app.rally_service_open = true;
            app.rally_service_selected = 0;
            app.rally_service_repaired = false;
            app.rally_notice.clear();
          }
        }
        if (app.race && app.race->session().started()) {
          app.race_top_speed = std::max(app.race_top_speed, std::fabs(vehicle.speed()));
          race::RaceBonusInputs bonus_in;
          bonus_in.assists_pct = race::assist_difficulty_bonus_percent(app.race->assists());
          bonus_in.skills_points = app.skill_tracker ? app.skill_tracker->race_skills_banked() : 0;
          bonus_in.wristband_level = app.collected_wristband;
          bonus_in.legacy_fm4_xp = app.legacy_fm4_progression;
          // BEAT THE STAR: the first star driver (AIPlayers.Nemesis) in the
          // field, at its running place when the player takes the flag.
          {
            const auto& opps = app.race->event().opponents;
            const auto& ses = app.race->session();
            const size_t field_ai = std::min(opps.size(), app.race->player_index());
            for (size_t k = 0; k < field_ai && k < ses.size(); ++k) {
              if (opps[k].nemesis) {
                bonus_in.star_place = ses.car(k).place;
                break;
              }
            }
          }
          bonus_in.mileage = app.post_race_options.for_mode(
              race::post_race_mode(app.race->event().career_type, app.race->rivals_solo()));
          // RACE-05: the award is computed at the line and held; the career,
          // the garage and the save change only when the player leaves the
          // finish banner (race_continue). Headless runs have no banner.
          if (app.race->award(app.career, app.wristbands, app.race_top_speed, bonus_in)) {
            if (app.skill_tracker) app.skill_tracker->reset_race_skills_banked();
            if (app.headless) commit_race_award();
          }
        }
        // game_free_roam_flow.xml: a wristband ready to collect enters radiodj.special_wristband
        // (CongratsWinnersOfWristband<n>), once per wristband.
        // Seeded on the first step after the game loads, race or not.
        if (app.notified_wristband == -2 && app.have_career_catalog)
          app.notified_wristband = app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband);
        // Not in the career's opening: its rewards hand over the yellow band (no
        // toast or DJ line for it before; first30a captures).
        if (!app.race && app.have_career_catalog && !app.campaign.ftc_active()) {
          const int ready = app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband);
          if (ready >= 0) app.audio_host.radio_wristband_ready(ready);
          if (ready >= 0 && ready != app.notified_wristband) {
            app.notified_wristband = ready;
            popup = Popup::notify(NotifyKind::kWristbandAvailable, vehicle.time());
          }
          // A street race hub's RadioDJRequests special (StreetRaceOwnerHub<n>) when it unlocks.
          if (std::fabs(vehicle.time() - app.street_hub_radio_check_at) >= 1.0) {
            app.street_hub_radio_check_at = vehicle.time();
            for (const auto& hub : app.career_catalog.hubs) {
              const bool open = app.career_catalog.is_hub_unlocked(hub, app.career, app.collected_wristband);
              auto [it, first] = app.street_hub_radio_unlocked.try_emplace(hub.id, open);
              if (!first && open && !it->second)
                for (const auto& ev : hub.radio_specials) app.audio_host.radio_trigger_special(ev);
              it->second = open;
            }
          }
        }
        // Free roam's discoveries wait for the player's own driving.
        const bool smashed_sign = app.campaign.cutscene_drives_player() ? false : free_roam_step();
        if (smashed_sign) club_pending.smashables.push_back("sign");
        if (host.free_roam && host.free_roam->hidden()) app.smashables.sync_barns_from_hidden(*host.free_roam->hidden());
        bool smashed_prop = smashed_sign;
        freeroam::SmashObjectType smashed_type = freeroam::SmashObjectType::kGeneric;
        // FORMAT-PHYSICS-DEFINITIONS: step 384-body smashable prop pool & barn doors.
        if (!app.bench || app.bench_full_fx) {
          thread_local std::vector<world::SmashEvent> sm_events;
          sm_events.clear();
          app.smashables.step(vehicle, *host.ground, float(drive::Vehicle::kStep), sm_events);
          for (const auto& sev : sm_events) {
            smashed_prop = true;
            step_object_impact_accum = std::max(step_object_impact_accum, std::max(0.25, std::fabs(vehicle.speed()) / 25.0));
            // Hide intact static world prop immediately on smash and persist across regathers.
            host.showroom.hide_world_props_near(sev.pos, sev.hide_radius);
            host.world_gather_options.hidden_positions = app.smashables.broken_hidden_positions();
            if (host.streamer) host.streamer->set_hidden_positions(host.world_gather_options.hidden_positions);
            const std::string stype = freeroam::club::classify_smashable_type(sev.prop_stem, sev.object_type_index);
            club_pending.smashables.push_back(stype);
            const std::string stem_l = lower_ascii(sev.prop_stem);
            if (stype == "fence" || stem_l.find("fence") != std::string::npos)
              smashed_type = freeroam::SmashObjectType::kFence;
            else if (stem_l.find("mailbox") != std::string::npos || stem_l.find("postbox") != std::string::npos)
              smashed_type = freeroam::SmashObjectType::kMailbox;
            else if (stem_l.find("roadclosed") != std::string::npos || stem_l.find("road_closed") != std::string::npos)
              smashed_type = freeroam::SmashObjectType::kRoadClosedSign;
            else if (stem_l.find("trash") != std::string::npos || stem_l.find("bin") != std::string::npos ||
                     stem_l.find("garbage") != std::string::npos)
              smashed_type = freeroam::SmashObjectType::kTrashCan;
            else if (stem_l.find("log") != std::string::npos || stem_l.find("lumber") != std::string::npos)
              smashed_type = freeroam::SmashObjectType::kLogPiles;
            else if (stem_l.find("fruit") != std::string::npos || stem_l.find("produce") != std::string::npos)
              smashed_type = freeroam::SmashObjectType::kFruitCart;
            else if (stem_l.find("hydrant") != std::string::npos)
              smashed_type = freeroam::SmashObjectType::kFireHydrant;
            host.particle_sys.spawn_smashable_burst(sev.prop_stem, sev.pos, sev.impulse_vel);
            audio::CollisionHit bhit{};
            bhit.rel_speed_mps = float(std::fabs(vehicle.speed()));
            bhit.pos = {sev.pos[0], sev.pos[1], sev.pos[2]};
            bhit.is_break = true;
            bhit.smash_material = classify_smash_material(stem_l, stype, bhit.surface_hint);
            if (bhit.smash_material == audio::SmashMaterial::kMetal) {
              host.particle_sys.spawn_collision_impact(
                  {sev.pos[0], sev.pos[1] + 0.6f, sev.pos[2]}, {0.0f, 1.0f, 0.0f}, sev.impulse_vel,
                  float(std::clamp(std::fabs(vehicle.speed()) * 0.45, 3.0, 12.0)), true);
            }
            app.audio_host.trigger_collision(bhit);
            if (sev.object_type_index == 1) app.audio_host.trigger_ui(audio::UiCue::kSignSmash);
          }
        }
        // SPEC-VFX-AND-PARTICLES: step tyre smoke/dust/gravel/sparks/backfire,
        // world ambient emitters, and persistent/fading skidmarks & ruts.
        if (!app.bench || app.bench_full_fx) {
          const drive::Vec3 vpos = vehicle.position();
          const drive::Vec3 vvel = vehicle.velocity();
          const drive::Quat vrot = vehicle.orientation();
          const drive::Vec3 vfwd = drive::rotate(vrot, {0, 0, 1});
          const drive::Vec3 vright = drive::rotate(vrot, {1, 0, 0});
          const drive::Vec3 vup = drive::rotate(vrot, {0, 1, 0});
          const auto& surfs = host.collision.surfaces();

          world::VehicleVfxInput vin{};
          vin.car_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
          vin.car_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
          vin.car_forward = {float(vfwd.x), float(vfwd.y), float(vfwd.z)};
          vin.car_right = {float(vright.x), float(vright.y), float(vright.z)};
          vin.car_up = {float(vup.x), float(vup.y), float(vup.z)};
          struct ExhaustTipCache {
            const Showroom* sr = nullptr;
            std::string car;
            size_t draws = 0;
            std::vector<std::array<float, 3>> tips;
          };
          thread_local ExhaustTipCache tip_cache;
          const size_t sr_draws = host.showroom.draw_count();
          if (tip_cache.sr != &host.showroom || tip_cache.car != app.car || tip_cache.draws != sr_draws) {
            tip_cache.sr = &host.showroom;
            tip_cache.car = app.car;
            tip_cache.draws = sr_draws;
            tip_cache.tips = host.showroom.exhaust_tips_local();
          }
          const auto& local_tips = tip_cache.tips;
          if (!local_tips.empty()) {
            const size_t tip_n = std::min(local_tips.size(), vin.exhaust_tips.size());
            for (size_t ti = 0; ti < tip_n; ++ti) {
              const auto& lt = local_tips[ti];
              const drive::Vec3 wt = vpos + drive::rotate(vrot, {double(lt[0]), double(lt[1]), double(lt[2])});
              vin.exhaust_tips[ti] = {float(wt.x), float(wt.y), float(wt.z)};
            }
            vin.exhaust_tip_count = int(tip_n);
          } else {
            const float half_len = float(std::max(1.4, vehicle.params().size.z * 0.5));
            const float half_w = float(std::max(0.6, vehicle.params().size.x * 0.5));
            const drive::Vec3 ex_l = vpos + drive::rotate(vrot, {-half_w * 0.42, -0.22, -half_len - 0.05});
            const drive::Vec3 ex_r = vpos + drive::rotate(vrot, {half_w * 0.42, -0.22, -half_len - 0.05});
            vin.exhaust_tips[0] = {float(ex_l.x), float(ex_l.y), float(ex_l.z)};
            vin.exhaust_tips[1] = {float(ex_r.x), float(ex_r.y), float(ex_r.z)};
            vin.exhaust_tip_count = 2;
          }

          const float idle_r = float(std::max(600.0, vehicle.params().idle_rpm));
          const float red_r = float(std::max(idle_r + 1000.0, vehicle.params().redline_rpm));
          vin.norm_rpm = std::clamp((float(vehicle.rpm()) - idle_r) / (red_r - idle_r), 0.0f, 1.0f);
          vin.throttle = float(in.throttle);
          vin.upshift_event = (vehicle.gear() > host.last_vfx_gear && vehicle.gear() >= 2);
          host.last_vfx_gear = vehicle.gear();
          vin.backfire_level = std::clamp(
              std::max({audio_upgrades.exhaust, audio_camshaft_level, vehicle.params().has_turbo ? 1 : 0}), 0, 3);
          const uint32_t cur_bf = app.audio_host.telemetry().backfire_count;
          if (cur_bf > host.last_audio_backfire_count) {
            vin.audio_backfire_pulses = cur_bf - host.last_audio_backfire_count;
          }
          host.last_audio_backfire_count = cur_bf;

          for (int wi = 0; wi < drive::kWheels; ++wi) {
            const auto& w = vehicle.wheels()[size_t(wi)];
            auto& wv = vin.wheels[size_t(wi)];
            wv.grounded = w.contact;
            if (!w.contact) {
              host.skid_sys.break_wheel(size_t(wi));
              continue;
            }
            const double tr = vehicle.params().tyre_radius[size_t(wi)] > 0 ? vehicle.params().tyre_radius[size_t(wi)] : 0.33;
            const drive::Vec3 patch = vpos + drive::rotate(vrot, {w.centre.x, w.centre.y - tr, w.centre.z});
            const double cs = std::cos(w.steer), sn = std::sin(w.steer);
            const drive::Vec3 wr = drive::rotate(vrot, {cs, 0, -sn});
            const drive::Vec3 wf = drive::rotate(vrot, {sn, 0, cs});
            wv.contact_pos = {float(patch.x), float(patch.y), float(patch.z)};
            wv.surface_normal = {float(vup.x), float(vup.y), float(vup.z)};
            wv.wheel_right = {float(wr.x), float(wr.y), float(wr.z)};
            wv.wheel_forward = {float(wf.x), float(wf.y), float(wf.z)};
            wv.tyre_width_m = 0.25f;
            wv.slip_combined = float(std::hypot(w.slip_ratio, w.slip_angle));
            wv.wheel_speed_mps = float(std::max(std::fabs(w.omega * tr), std::fabs(vehicle.speed())));
            wv.surface_index = w.surface;
            if (w.surface >= 0 && size_t(w.surface) < surfs.size()) {
              wv.surface = surfs[size_t(w.surface)];
            } else {
              // Synthetic fallback (asphalt) when driving on flat ground without collision surfaces.
              wv.surface.skid_a = 200;
              wv.surface.skid_texture_index = 0;
              wv.surface.skid_min_intensity = 0.0f;
              wv.surface.smoke_weight = 1.0f;
              wv.surface.should_spark = true;
            }
            host.skid_sys.update_wheel(size_t(wi), wv);
          }

          // Body scrape / impact sparks on ShouldSpark surfaces.
          const auto bc = vehicle.strongest_body_contact();
          const double bc_speed = bc.force * drive::Vehicle::kStep / std::max(500.0, vehicle.params().mass);
          if (bc.force > 1500.0) {
            bool spk = true;
            if (bc.surface >= 0 && size_t(bc.surface) < surfs.size()) spk = surfs[size_t(bc.surface)].should_spark;
            vin.scraping = true;
            vin.scrape_pos = {float(bc.point.x), float(bc.point.y), float(bc.point.z)};
            vin.scrape_normal = {float(bc.normal.x), float(bc.normal.y), float(bc.normal.z)};
            vin.scrape_should_spark = spk;
            if (bc_speed > 3.5 && vehicle.time() - host.last_impact_vfx_time > 0.25) {
              host.last_impact_vfx_time = vehicle.time();
              host.particle_sys.spawn_collision_impact(vin.scrape_pos, vin.scrape_normal, vin.car_vel, float(bc_speed), spk);
            }
          }

          host.particle_sys.update_vehicle(vin, float(drive::Vehicle::kStep));
          host.particle_sys.update_world_emitters(vin.car_pos, float(app.tod_clock.hour()), float(drive::Vehicle::kStep));
          host.particle_sys.step(float(drive::Vehicle::kStep), {0.707f, 0.0f, 0.707f}, 1.45f);
        }
        // SPEC-SKILLS-POPULARITY: step driving skill detectors in free roam and active races.
        // No skills while the career's AI drives the car under a cutscene,
        // nor in the career's opening before PopularitySkillsFlow (none
        // score on the initial drive or the race to the festival in the
        // original; first30a captures).
        if (app.skill_tracker && !app.campaign.cutscene_drives_player() &&
            (!app.campaign.ftc_active() || app.campaign.ftc().skills_unlocked()) &&
            (!app.race || (app.race->session().started() && !app.race->player_finished()))) {
          freeroam::SkillInput si;
          si.dt = drive::Vehicle::kStep;
          si.time = vehicle.time();
          const drive::Vec3 vel = vehicle.velocity();
          const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
          const drive::Vec3 right = drive::rotate(vehicle.orientation(), {1, 0, 0});
          si.speed_ms = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
          si.forward_speed_ms = vel.x * fwd.x + vel.y * fwd.y + vel.z * fwd.z;
          si.lateral_speed_ms = vel.x * right.x + vel.y * right.y + vel.z * right.z;
          si.yaw_rate = vehicle.angular_velocity().y;
          si.handbrake = in.handbrake;
          if (app.have_career_catalog) {
            if (const auto* spec = app.career_catalog.find_car_by_media(app.car)) si.car_id = spec->car_id;
          }
          int on_ground = 0;
          double max_spin = 0;
          for (int wi = 0; wi < drive::kWheels; ++wi) {
            const auto& w = vehicle.wheels()[size_t(wi)];
            if (!w.contact) continue;
            ++on_ground;
            const double r = vehicle.params().tyre_radius[size_t(wi)] > 0 ? vehicle.params().tyre_radius[size_t(wi)] : 0.33;
            max_spin = std::max(max_spin, std::fabs(w.omega * r - si.forward_speed_ms));
          }
          si.wheels_on_ground = on_ground;
          si.wheel_spin = max_spin;
          const double mass = std::max(500.0, vehicle.params().mass);
          si.world_impact_ms = vehicle.strongest_body_contact().force * drive::Vehicle::kStep / mass;
          const double traffic_imp_s = (traffic && !reset_ghost_active) ? traffic->player_impulse() : 0.0;
          const double race_imp_s = app.race ? app.race->player_impulse() : 0.0;
          si.car_impact_ms = std::max({traffic_imp_s, race_imp_s, step_car_impact_accum}) / mass;
          // Achievements 211 and 214 on a Horizon Rally stage: air covered,
          // and a crash as the skills count one (the chain-cancelling impact).
          if (app.rally_stage_signals.active && app.race && app.race->is_stage()) {
            app.saved.ach.rally_air_m +=
                app.rally_stage_signals.step(on_ground, std::hypot(vel.x, vel.z), drive::Vehicle::kStep);
            if (si.world_impact_ms >= app.skills_data.world_collision_threshold ||
                si.car_impact_ms >= app.skills_data.car_collision_threshold)
              app.rally_stage_signals.crashed = true;
          }
          si.smashed_object = smashed_prop;
          si.smashed_type = smashed_type;
          const drive::Vec3 at = vehicle.position();
          auto check_other = [&](double ox, double oz, double ospeed) {
            const double dx = ox - at.x, dz = oz - at.z;
            const double d = std::hypot(dx, dz);
            si.nearest_car_dist = std::min(si.nearest_car_dist, d);
            const double ahead = dx * fwd.x + dz * fwd.z;
            const double side = std::fabs(dx * right.x + dz * right.z);
            if (ahead > 4.0 && ahead < 28.0 && side < 2.2 && ospeed > 15.0)
              si.draft_factor = std::max(si.draft_factor, std::clamp(1.0 - (ahead - 4.0) / 24.0, 0.0, 1.0));
          };
          if (traffic && traffic->cars())
            for (const auto& tc : *traffic->cars()) check_other(tc.position.x, tc.position.z, tc.speed);
          if (festival_mgr)
            for (const auto& fc : festival_mgr->cars()) check_other(fc.position.x, fc.position.z, fc.speed);
          si.draft_factor = std::max(si.draft_factor, vehicle.draft().strength);
          if (app.race) {
            std::vector<carview::RaceView::OpponentTag> otags;
            app.race->opponent_tags(otags);
            for (const auto& ot : otags) check_other(ot.x, ot.z, ot.speed);
            const int cur_place = app.race->player_place();
            if (prev_race_place > 0 && cur_place < prev_race_place && app.race->session().clock() > 2.0) {
              si.passed_opponent = true;
              si.pass_speed_diff_mph = std::max(5.0, si.speed_ms * 0.25);
            }
            prev_race_place = cur_place;
          }
          const int64_t prev_max_skills = app.skill_tracker->max_skills_score();
          int prev_pop_rank = app.skill_tracker->popularity_rank();
          for (const auto& sev : app.skill_tracker->step(si)) {
            if (sev.kind == freeroam::SkillEvent::kSkillAwarded) {
              club_pending.skills.push_back(
                  {sev.skill_id, freeroam::club::skill_tracker_grade_to_club_grade(sev.grade)});
              const int cur_mult = int(app.skill_tracker->hud().multiplier);
              if (cur_mult > prev_audio_skill_mult) {
                app.audio_host.trigger_ui(audio::UiCue::kSkillMultiplier);
              } else {
                app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
              }
              prev_audio_skill_mult = std::max(1, cur_mult);
            } else if (sev.kind == freeroam::SkillEvent::kChainBanked) {
              club_pending.skill_chain_completed = true;
              app.audio_host.trigger_ui(audio::UiCue::kSkillBank);
              prev_audio_skill_mult = 1;
            } else if (sev.kind == freeroam::SkillEvent::kChainLost) {
              club_pending.skill_chain_completed = true;
              app.audio_host.trigger_ui(audio::UiCue::kSkillBreak);
              prev_audio_skill_mult = 1;
            }
            if (sev.kind == freeroam::SkillEvent::kFameSponsor && sev.credits > 0) {
              const int64_t scr = slice12.trainer.pacing.scale_credits(sev.credits);
              app.career.credits = std::min<int64_t>(999999999, app.career.credits + scr);
              app.career.total_winnings += scr;
              popup = sponsor_popup(*this, sev.skill_id, fame_tier(*this, sev.rank), scr);
              save_due = true;
            } else if (sev.kind == freeroam::SkillEvent::kSponsorChallenge && sev.credits > 0) {
              const int64_t scr = slice12.trainer.pacing.scale_credits(sev.credits);
              app.career.credits = std::min<int64_t>(999999999, app.career.credits + scr);
              app.career.total_winnings += scr;
              popup = sponsor_popup(*this, sev.skill_id, sev.rank, scr);
              save_due = true;
            } else if (sev.kind == freeroam::SkillEvent::kRankUp) {
              const int old_rank = std::max(prev_pop_rank, sev.rank + 1);
              app.audio_host.trigger_ui(audio::UiCue::kRankUp);
              if (!sev.radio_festival_update.empty()) app.audio_host.radio_trigger_festival_update(sev.radio_festival_update);
              app.campaign.on_popularity_rank_changed(old_rank, sev.rank, &app.audio_host);
              // Notify if any Showcase event unlocked across a single- or multi-rank jump!
              if (app.have_career_catalog) {
                for (const auto& cev : app.career_catalog.events)
                  if (cev.style == 3 && cev.popularity_rank_req > 0 &&
                      sev.rank <= cev.popularity_rank_req && cev.popularity_rank_req < old_rank) {
                    popup = Popup::notify(NotifyKind::kShowcaseUnlocked, vehicle.time());
                  }
              }
              prev_pop_rank = std::min(prev_pop_rank, sev.rank);
              save_due = true;
            }
          }
          if (app.skill_tracker->max_skills_score() > prev_max_skills) {
            int64_t cid = app.skill_tracker->max_skills_car();
            if (cid <= 0 && app.pre_loaner_car.empty() && app.have_garage) {
              if (const garage::Car* sc = app.career_garage.selected_car()) cid = sc->car_id;
            }
            if (cid <= 0 && app.have_career_catalog) {
              if (const auto* spec = app.career_catalog.find_car_by_media(app.car)) cid = spec->car_id;
            }
            if (cid > 0) app.max_skills_car = cid;
          }
        }
        // Speed Traps & Speed Zones (free-roam gate crossings, camera flash, audio cue, PB persistence).
        // Quiet during closed races and first-time career (unless --demo-speed-trap is active).
        if (!app.race && app.speed_tracker && (!app.campaign.ftc_active() || app.demo_speed_trap)) {
          const drive::Vec3 sp_at = vehicle.position();
          if (!have_prev_speed_pos) {
            prev_speed_x = sp_at.x;
            prev_speed_z = sp_at.z;
            have_prev_speed_pos = true;
          }
          // Two passes: ~144.3 mph at 4 s (a new PB), then ~134.2 mph at 13 s
          // (short of it: the personal best line).
          if (app.demo_speed_trap && demo_speed_trap_passes < 2 &&
              vehicle.time() >= (demo_speed_trap_passes == 0 ? 4.0 : 13.0) && !app.speed_data.traps.empty()) {
            const double mps = demo_speed_trap_passes == 0 ? 64.5 : 60.0;
            ++demo_speed_trap_passes;
            const auto& g = app.speed_data.traps.front().gate;
            const double gx = g.right[0] - g.left[0], gz = g.right[2] - g.left[2];
            const double gl = std::max(1.0, std::hypot(gx, gz));
            const double nx = -gz / gl, nz = gx / gl;
            app.speed_tracker->step(drive::Vehicle::kStep, vehicle.time(), g.center[0] - nx * 4.0, g.center[2] - nz * 4.0,
                                g.center[0] + nx * 4.0, g.center[2] + nz * 4.0, mps);
          }
          // R3-CAM-11: Feed live off-road state to SpeedActivityTracker before stepping Speed Zones.
          {
            const auto& surfs = host.collision.surfaces();
            int grounded_wheels = 0;
            int offroad_wheels = 0;
            for (int wi = 0; wi < drive::kWheels; ++wi) {
              const auto& w = vehicle.wheels()[size_t(wi)];
              if (!w.contact) continue;
              ++grounded_wheels;
              if (w.surface >= 0 && size_t(w.surface) < surfs.size()) {
                const auto& sf = surfs[size_t(w.surface)];
                if (sf.off_roadness >= 0.5f || sf.is_penalty_surface) ++offroad_wheels;
              }
            }
            const bool is_off_road =
                (grounded_wheels >= 2 && offroad_wheels >= 3) || (grounded_wheels > 0 && offroad_wheels == grounded_wheels);
            app.speed_tracker->set_off_road(is_off_road);
          }
          const auto speed_evs = app.speed_tracker->step(drive::Vehicle::kStep, vehicle.time(), prev_speed_x, prev_speed_z,
                                                     sp_at.x, sp_at.z, vehicle.speed());
          prev_speed_x = sp_at.x;
          prev_speed_z = sp_at.z;
          for (const auto& ev : speed_evs) {
            if (ev.kind == freeroam::SpeedActivityEvent::kZoneEntered) {
              app.audio_host.set_speed_zone_active(true);
            } else if (ev.kind == freeroam::SpeedActivityEvent::kZoneCancelled) {
              app.audio_host.set_speed_zone_active(false);
            } else if (ev.kind == freeroam::SpeedActivityEvent::kTrapTriggered ||
                       ev.kind == freeroam::SpeedActivityEvent::kZoneCompleted) {
              app.audio_host.trigger_ui(audio::UiCue::kSpeedCamera);
              const bool is_zone = (ev.kind == freeroam::SpeedActivityEvent::kZoneCompleted);
              if (is_zone) app.audio_host.set_speed_zone_active(false);
              app.audio_host.trigger_speed_cam();
              int sb_id = 0;
              if (is_zone) {
                if (const auto* z = app.speed_data.find_zone(ev.id)) sb_id = z->scoreboard_id;
              } else {
                if (const auto* t = app.speed_data.find_trap(ev.id)) sb_id = t->scoreboard_id;
              }
              club_pending.cameras.push_back({is_zone, sb_id, ev.id, ev.speed_mph});
              if (ev.is_new_pb && app.legacy_fm4_progression && app.have_prog_tables) {
                race::apply_progression_xp(app.career, 750, current_make_id(), 350, app.prog_tables);
              }
              // Speed traps and zones rank the speed on the leaderboards;
              // no popularity is paid for them (probable, SPEC Q114).
              std::printf("speed activity: %s %.1f mph%s\n", ev.label.c_str(), ev.speed_mph,
                          ev.is_new_pb ? " (NEW PB!)" : "");
              save_due = true;
            }
          }
        } else {
          have_prev_speed_pos = false;
        }
        if (app.freeroam_challenges && !app.race && !app.campaign.cutscene_drives_player() &&
            !app.campaign.flow().is_onboarding())
          step_freeroam_challenges(*this);  // before club_step() clears this step's skills and cameras
        club_step();  // evaluates all active 1000 Club challenges on full step signals
        if (!host.streamer) return true;
        // Headless runs put a car that is stuck or rolled back on the
        // nearest road; windowed free-roam also auto-recovers sustained rollovers (~2.0 s).
        if (app.headless && !app.race && host.follower.stuck(vehicle, in)) {
          const drive::Vec3 at = vehicle.position();
          double away = 0;
          if (safe_reset_ok && safe_reset.place(*host.streamer, vehicle, &away)) {
            rig_accel.reset();
            std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away\n", vehicle.time(), at.x,
                        at.y, at.z, away);
          } else if (world::Spawn sp; world::recover(*host.streamer, vehicle, &sp)) {
            rig_accel.reset();
            std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away (nearest road "
                        "triangle)\n", vehicle.time(), at.x, at.y, at.z, sp.distance);
          }
        } else if (!app.headless && !app.race) {
          if (g_rollover_state.owner != this || vehicle.time() < g_rollover_state.last_sim_time - 1e-6) {
            g_rollover_state.owner = this;
            g_rollover_state.rolled_s = 0.0;
          }
          g_rollover_state.last_sim_time = vehicle.time();
          if (vehicle.is_rolled_over() && reset_ux.phase == ResetToRoadState::Phase::kIdle) {
            g_rollover_state.rolled_s += drive::Vehicle::kStep;
            if (g_rollover_state.rolled_s >= 2.0) {
              g_rollover_state.rolled_s = 0.0;
              const auto pre_pose = vehicle.pose();
              const drive::Vec3 at = vehicle.position();
              double away = 0.0;
              world::Spawn sp{};
              bool placed = false;
              if (safe_reset_ok && safe_reset.place(*host.streamer, vehicle, &away)) {
                placed = true;
              } else if (world::recover(*host.streamer, vehicle, &sp)) {
                away = sp.distance;
                placed = true;
              } else {
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                double gy = at.y;
                if (host.ground) world::ground_height(*host.ground, at.x, at.z, at.y + 4.0, gy);
                vehicle.place({at.x, gy + 0.05, at.z}, std::atan2(f.x, f.z));
                placed = true;
              }
              if (placed) {
                const auto tgt_pose = vehicle.pose();
                begin_smooth_reset(pre_pose, tgt_pose, "CAR ROLLED OVER — RESET TO ROAD");
                std::printf("t %.2f s: rollover recovery at %.1f %.1f %.1f (%.1f m to road)\n", vehicle.time(), at.x,
                            at.y, at.z, away);
              }
            }
          } else {
            g_rollover_state.rolled_s = 0.0;
          }
        }
        // Off the collision (the .fiz has holes outside the drivable strips):
        // a car 20 m below where its wheels last touched goes back on the
        // road, in the window too (ours; the game keeps cars out with walls).
        {
          bool touching = false;
          for (const auto& w : vehicle.wheels()) touching |= w.contact;
          const drive::Vec3 p = vehicle.position();
          if (touching || !have_touch) last_touch_y = p.y, have_touch = true;
          else if (p.y < last_touch_y - 20 && reset_ux.phase == ResetToRoadState::Phase::kIdle) {
            const auto pre_pose = vehicle.pose();
            double away = 0;
            world::Spawn sp;
            const bool placed = (safe_reset_ok && safe_reset.place(*host.streamer, vehicle, &away)) ||
                                (world::recover(*host.streamer, vehicle, &sp) && ((away = sp.distance), true));
            std::printf("t %.2f s: fell through the ground at %.1f %.1f %.1f (no collision there)%s\n", vehicle.time(),
                        p.x, p.y, p.z, placed ? ", put back on the road" : "");
            if (placed) {
              if (!app.headless && !app.bench) {
                const auto tgt_pose = vehicle.pose();
                begin_smooth_reset(pre_pose, tgt_pose, "OFF THE WORLD — RESET TO ROAD");
              } else {
                rig_accel.reset();
                notice = "off the world: put back on the road";
                notice_until = vehicle.time() + 3;
              }
            }
            last_touch_y = vehicle.position().y;
          }
        }
        if (reset_ux.phase == ResetToRoadState::Phase::kIdle) {
          reset_ux.last_live_pose = vehicle.pose();
          reset_ux.has_last_live_pose = true;
        }
        const auto st_t0 = std::chrono::steady_clock::now();
        step_ai_traffic_ms += std::chrono::duration<double, std::milli>(st_t0 - ai_t0).count();
        bool changed = false;
        const drive::Vec3 at = vehicle.position();
        if (!host.streamer->update(at.x, at.z, &changed, &app.error)) return false;
        if (changed) {
          using clock = std::chrono::steady_clock;
          const auto t0 = clock::now();
          host.ground = &host.streamer->ground();
          host.follower.set_ground(&host.streamer->ground());  // its road points: set_staged_world
          const auto t1 = clock::now();
          if (!host.set_staged_world()) return false;
          const auto t2 = clock::now();
          std::printf("t %.2f s: gathered again around %.1f %.1f (%zu triangles, %.2f s; on the step: ground "
                      "%.1f ms, scenery hand-over %.1f ms)\n",
                      vehicle.time(), host.streamer->centre_x(), host.streamer->centre_z(), host.streamer->last_mesh_triangles(),
                      host.streamer->last_gather_seconds(), std::chrono::duration<double, std::milli>(t1 - t0).count(),
                      std::chrono::duration<double, std::milli>(t2 - t1).count());
        }
        bool swapped = false;
        const auto p0 = std::chrono::steady_clock::now();
        host.showroom.poll_world(false, &swapped);
        if (swapped && host.streamer_async)
          std::printf("t %.2f s: scenery around %.1f %.1f uploaded (swap %.1f ms)\n", vehicle.time(),
                      host.showroom.world_origin_x(), host.showroom.world_origin_z(),
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count());
        step_world_stream_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st_t0).count();
        return true;
}

bool DriveSession::commit_race_award() {
  if (!app.race) return false;
  const int64_t bal_before = app.career.credits;
  const int pop_before = app.current_pop_rank();
  const race::RaceAward* a = app.race->commit_award(app.career);
  if (!a) return false;
  // Achievement 183: a win against opponents (not a solo Rivals run) on Hard
  // (or harder) opponent difficulty.
  if (app.race->player_place() == 1 && app.race->session().size() > 1 && !app.active_rivals.active &&
      app.race_options.difficulty >= race::Difficulty::kHard)
    ++app.saved.ach.hard_race_wins;
  if (slice12.trainer.pacing.modifies_economy()) {
    const int64_t scaled_cr = slice12.trainer.pacing.scale_credits(a->credits);
    const int64_t extra_cr = scaled_cr - a->credits;
    if (extra_cr != 0) {
      app.career.credits = std::clamp<int64_t>(app.career.credits + extra_cr, 0, 999999999);
      app.career.total_winnings = std::max<int64_t>(0, app.career.total_winnings + extra_cr);
    }
    const int scaled_wb = slice12.trainer.pacing.scale_points(a->wristband_points);
    const int extra_wb = scaled_wb - a->wristband_points;
    if (extra_wb != 0) app.career.wristband_points = std::max(0, app.career.wristband_points + extra_wb);
  }
  if (app.legacy_fm4_progression && app.have_prog_tables) {
    const int drv_xp = slice12.trainer.pacing.scale_points(a->driver_xp);
    const int aff_xp = slice12.trainer.pacing.scale_points(a->affinity_xp);
    // IsCarInstalledAndPurchased: a car the disc and its packages carry.
    const auto installed = [&](int64_t cid) {
      return (app.have_career_catalog && app.career_catalog.find_car(cid)) || app.baked_ids.count(cid) > 0;
    };
    auto pu = race::apply_progression_xp(app.career, drv_xp, current_make_id(), aff_xp, app.prog_tables,
                                         installed);
    // A pick-one list (two or more cars): the engine has no pick screen yet
    // and gives the list's first (interim, SPEC_QUESTIONS Q254).
    for (const auto& pick : pu.gift_picks)
      if (!pick.empty()) pu.gift_cars.push_back(pick.front());
    for (int64_t gcid : pu.gift_cars) {
      const std::string gnm = app.award_garage_car(gcid, 0, false, /*is_prize=*/true);
      if (!gnm.empty()) {
        popup = {"DRIVER LEVEL " + std::to_string(pu.new_driver_level) + " GIFT CAR!", gnm, vehicle.time(),
                 -1, carview::ui::rgba(247, 255, 38)};
        app.garage_picker();
      }
    }
    if (pu.affinity_bonus_credits > 0) {
      app.sponsor_banner_title = "AFFINITY LEVEL " + std::to_string(pu.new_affinity_level);
      app.sponsor_banner_detail = "+" + carview::thousands(pu.affinity_bonus_credits) + " Cr  (" +
                              std::to_string(int(std::lround(pu.affinity_discount * 100.0))) + "% UPGRADE DISC)";
      app.sponsor_banner_at = vehicle.time();
    }
  }
  const int lvl = app.wristbands.level_index(app.career.wristband_points);
  std::string band_label = "no wristband";
  if (lvl >= 0 && size_t(lvl) < app.wristbands.levels.size()) {
    band_label = carview::game_text(app.wristbands.levels[size_t(lvl)].second);
    if (band_label.empty() || band_label.rfind("_&", 0) == 0) {
      if (const auto* ms = race::find_wristband_milestone(lvl)) {
        band_label = std::string(ms->colour_key) + " Wristband";
      } else {
        band_label = "Wristband " + std::to_string(lvl + 1);
      }
    } else {
      std::string low = band_label;
      for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
      if (low.find("wristband") == std::string::npos) band_label += " Wristband";
    }
  }
  std::printf("race: award %lld CR (base %lld, mileage %lld, diff +%d%% %lld, assists %lld, skills %lld, nemesis %lld), %d wristband points (total %d, %s), credits %lld%s%s\n",
              (long long)a->credits, (long long)a->base_credits, (long long)a->mileage_bonus,
              a->difficulty_pct, (long long)a->difficulty_bonus, (long long)a->assists_bonus,
              (long long)a->skills_bonus, (long long)a->nemesis_bonus, a->wristband_points,
              app.career.wristband_points, band_label.c_str(),
              (long long)app.career.credits, a->first_completion ? ", first completion" : "",
              a->prize_car ? (", prize car " + std::to_string(a->prize_car)).c_str() : "");
  if (a->prize_car > 0) {
    int prize_col = 0, prize_class = -1;
    if (const auto* cev = app.career_catalog.find_event(app.race_event)) {
      prize_col = cev->prize_car_colour;
      prize_class = cev->prize_car_upgrade_class;
    }
    won_car_name = app.award_garage_car(a->prize_car, prize_col, false, /*is_prize=*/true, prize_class);
    app.garage_picker();
  }
  // Dynamic difficulty (AISkillAdjustments): update profile Main/AISkill
  // after finished races in STOCK mode (skipped in Fair / Hard+).
  const double pl_t = app.race->session().car(app.race->player_index()).finish;
  const double ai_ref = app.race->ai_reference_time();
  const race::AiMode eff_mode =
      race::is_pinyon_hard(app.race_options.difficulty, app.race_options.ai_mode)
          ? race::AiMode::kHard
          : (app.race_options.rubberband ? app.race_options.ai_mode : race::AiMode::kFair);
  app.dynamic_ai_skill = race::update_dynamic_ai_skill(
      app.dynamic_ai_skill, pl_t, ai_ref, app.career.num_races, eff_mode, app.race->event().skill_adjustments);
  if (!app.headless) {
    const race::CareerEvent* cev = app.have_career_catalog ? app.career_catalog.find_event(app.race_event) : nullptr;
    const int pl = app.race->player_place();
    const int ncars = app.race->is_showcase() ? 2 : int(app.race->player_index()) + 1;
    // R3-CAM-03: Capture actual RaceView standings (real drivers, cars, class/PI, finish clock/gap/best lap)
    // for Post-Race Stage 1 RACE RESULTS instead of fabricating boss names or fake +0.92s gaps.
    std::vector<race::RaceFinishStanding> finish_standings;
    const RaceHud rh = app.race->hud_info();
    if (app.race->is_showcase()) {
      race::RaceFinishStanding p_st;
      p_st.place = pl;
      p_st.is_player = true;
      p_st.finish_time_s = rh.finish_time;
      p_st.best_lap_s = rh.best_lap;
      if (!rh.rows.empty()) {
        p_st.driver_name = rh.rows.front().name;
        p_st.car_name = rh.rows.front().car;
        p_st.class_name = rh.rows.front().car_class;
        p_st.pi = rh.rows.front().pi;
      } else {
        p_st.driver_name = "YOU";
        p_st.car_name = carview::car_name(app.car);
      }
      const auto& sc_opp = app.race->showcase();
      race::RaceFinishStanding a_st;
      a_st.place = (pl == 1 ? 2 : 1);
      a_st.driver_name = sc_opp.spec().display_name;
      a_st.car_name = sc_opp.spec().vehicle_name;  // AirChallenges IDS_VehicleN
      a_st.finish_time_s = sc_opp.pose().finished ? sc_opp.pose().finish_time : -1.0;
      const double est_gap =
          (a_st.finish_time_s > 0.0 && p_st.finish_time_s > 0.0)
              ? std::fabs(a_st.finish_time_s - p_st.finish_time_s)
              : (std::fabs(rh.showcase_gap_m) / std::max(10.0, sc_opp.pose().speed_mps));
      p_st.gap_s = (pl == 1) ? 0.0 : est_gap;
      a_st.gap_s = (pl == 1) ? est_gap : 0.0;
      if (pl == 1) {
        finish_standings.push_back(std::move(p_st));
        finish_standings.push_back(std::move(a_st));
      } else {
        finish_standings.push_back(std::move(a_st));
        finish_standings.push_back(std::move(p_st));
      }
    } else {
      const bool event_is_boss = cev && race::find_boss_by_event(cev->horizon_id) != nullptr;
      double lead_finish_t = -1.0;
      for (const auto& r : rh.rows) {
        if (r.place == 1 && r.finished && r.time > 0.0) {
          lead_finish_t = r.time;
          break;
        }
      }
      finish_standings.reserve(rh.rows.size());
      for (const auto& r : rh.rows) {
        race::RaceFinishStanding st;
        st.place = r.place;
        st.driver_name = !r.name.empty() ? r.name : (r.player ? "YOU" : "HORIZON DRIVER");
        st.car_name = r.car;
        st.class_name = r.car_class;
        st.pi = r.pi;
        st.finish_time_s = r.finished ? r.time : -1.0;
        st.best_lap_s = r.best_lap;
        st.is_player = r.player;
        st.is_boss = (!r.player && event_is_boss);
        if (r.place == 1) {
          st.gap_s = 0.0;
        } else if (lead_finish_t > 0.0 && r.finished && r.time > 0.0) {
          st.gap_s = std::max(0.0, r.time - lead_finish_t);
        } else {
          st.gap_s = std::max(0.0, r.gap);
        }
        finish_standings.push_back(std::move(st));
      }
    }
    const auto camp_act = app.campaign.on_race_finished(
        cev, *a, pl, ncars, pop_before, app.current_pop_rank(), bal_before, app.career.credits,
        app.career.wristband_points, app.collected_wristband, won_car_name,
        app.have_career_catalog ? &app.career_catalog : nullptr, app.fmv_player, &app.audio_host,
        finish_standings);
    apply_campaign_action(camp_act);
    // The rewards screens are the race flow's (race_flow.h); the campaign
    // waits for on_post_race_done().
    // radiodj.xml: a wristband tier's festival results queue its "after n events" update.
    if (cev && cev->style == 1) app.audio_host.radio_festival_event_finished(cev->level, pl);
  }
  save_due = true;  // the window saves the award at once
  return true;
}

}  // namespace carview
