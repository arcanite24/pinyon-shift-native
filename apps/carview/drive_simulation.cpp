#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "world/road_follow.h"

namespace carview {

audio::EnginePlayer::Update DriveSession::sound_inputs(audio::View audio_view) {
        audio::EnginePlayer::Update u;
        const double rpm = vehicle.rpm(), thr = vehicle.engine_throttle();
        const double torque = vehicle.engine_torque(rpm, thr);
        const double full = vehicle.engine_torque(rpm, 1), closed = vehicle.engine_torque(rpm, 0);
        u.rpm = rpm;
        u.throttle = thr;
        u.pos_torque = torque > 0 && full > 0 ? torque / full : 0;
        u.neg_torque = torque < 0 && closed < 0 ? torque / closed : 0;
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
          case K::kRecalculating: cue = audio::SatNavCue::kRecalculating; break;
          case K::kUTurn: cue = audio::SatNavCue::kUTurn; break;
          case K::kArrivedNear: cue = audio::SatNavCue::kNearDestination; break;
          case K::kArrived:
            cue = audio::SatNavCue::kArrived;
            popup = {"DESTINATION REACHED", "YOU HAVE ARRIVED AT YOUR WAYPOINT", vehicle.time(), -1,
                     carview::ui::rgba(74, 238, 97)};
            break;
          case K::kTurnNow:
            switch (gev.turn) {
              case T::kLeft: cue = audio::SatNavCue::kTurnLeft; break;
              case T::kRight: cue = audio::SatNavCue::kTurnRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kSharpRight; break;
              default: play = false; break;
            }
            break;
          case K::kTurnNear:
            switch (gev.turn) {
              case T::kLeft: cue = audio::SatNavCue::kNearLeft; break;
              case T::kRight: cue = audio::SatNavCue::kNearRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kNearBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kNearBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kNearSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kNearSharpRight; break;
              default: play = false; break;
            }
            break;
          case K::kTurnFar:
            switch (gev.turn) {
              case T::kLeft: cue = audio::SatNavCue::kFarLeft; break;
              case T::kRight: cue = audio::SatNavCue::kFarRight; break;
              case T::kBearLeft: cue = audio::SatNavCue::kFarBearLeft; break;
              case T::kBearRight: cue = audio::SatNavCue::kFarBearRight; break;
              case T::kSharpLeft: cue = audio::SatNavCue::kFarSharpLeft; break;
              case T::kSharpRight: cue = audio::SatNavCue::kFarSharpRight; break;
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
          base_pts = int64_t(std::llround(sh.popularity_points)) + int64_t(sh.chain_points) * int64_t(sh.multiplier);
        }
        app.pr_stunt_session.start(st, base_pts);
        pr_stunt_has_prev = false;
        pr_stunt_photo_pressed = false;
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
        if (!app.race) {
          const drive::Vec3 at = vehicle.position();
          if (app.gps_nav.active()) {
            const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
            const double hdg = std::atan2(f.x, f.z);
            fire_satnav_event(app.gps_nav.update(at.x, at.z, hdg, std::fabs(vehicle.speed()), drive::Vehicle::kStep));
          }
          if (app.road_discovery.ready()) {
            for (const auto& rev : app.road_discovery.update(at.x, at.z)) {
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
              popup = {rb, rname, vehicle.time(), double(rev.discovered_count) / double(std::max(1, rev.total_count)),
                       carview::ui::rgba(74, 238, 97)};
              app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
              save_due = true;
            }
          }
          if (app.have_outposts) {
            const int disc_idx = app.outpost_progress.check_discovery(app.outposts, at.x, at.z);
            if (disc_idx >= 0 && size_t(disc_idx) < app.outposts.outposts.size()) {
              const auto& op = app.outposts.outposts[size_t(disc_idx)];
              if (app.skill_tracker) app.skill_tracker->add_popularity(500.0);
              const int dc = app.outpost_progress.discovered_count(app.outposts);
              popup = {"HORIZON OUTPOST DISCOVERED",
                       op.display_name + "  •  FAST TRAVEL UNLOCKED (" + std::to_string(dc) + "/" +
                           std::to_string(app.outposts.outposts.size()) + ")",
                       vehicle.time(), double(dc) / double(std::max<size_t>(1, app.outposts.outposts.size())),
                       carview::ui::rgba(255, 146, 3)};
              app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
              save_due = true;
            }
            if (app.pr_stunt_session.active()) {
              freeroam::PrStuntUpdate psu;
              psu.car_x = at.x;
              psu.car_z = at.z;
              psu.prev_x = pr_stunt_has_prev ? pr_stunt_prev_x : at.x;
              psu.prev_z = pr_stunt_has_prev ? pr_stunt_prev_z : at.z;
              psu.speed_mph = std::fabs(vehicle.speed()) * 2.2369363;
              if (app.skill_tracker) {
                const auto sh = app.skill_tracker->hud();
                psu.total_skill_points =
                    int64_t(std::llround(sh.popularity_points)) + int64_t(sh.chain_points) * int64_t(sh.multiplier);
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
                const bool first_time = app.outpost_progress.complete_stunt(st.id);
                const int64_t raw_cr = first_time ? st.reward_credits : st.reward_credits / 4;
                const int64_t cr = slice12.trainer.pacing.scale_credits(raw_cr);
                const int64_t pop = first_time ? st.reward_popularity : st.reward_popularity / 4;
                app.career.credits += cr;
                app.career.total_winnings += cr;
                if (app.skill_tracker) app.skill_tracker->add_popularity(double(pop));
                popup = {"PR STUNT COMPLETE!",
                         st.title + "   +" + carview::thousands(cr) + " Cr   +" + carview::thousands(pop) + " POP",
                         vehicle.time(),
                         double(app.outpost_progress.completed_stunt_count(app.outposts)) /
                             double(std::max<size_t>(1, app.outposts.stunts.size())),
                         carview::ui::rgba(74, 238, 97)};
                app.audio_host.trigger_ui(audio::UiCue::kSkillAward);
                app.gps_nav.clear();
                save_due = true;
              } else if (st_state == freeroam::PrStuntState::kFailed) {
                popup = {"PR STUNT FAILED", "TIME EXPIRED  •  RETRY AT THE HORIZON OUTPOST", vehicle.time(), -1,
                         carview::ui::rgba(255, 72, 72)};
                app.gps_nav.clear();
              }
            }
          }
        }
        if (!host.free_roam) return false;
        freeroam::CarState cs;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
        const double fl = std::hypot(f.x, f.z);
        cs.x = at.x, cs.y = at.y, cs.z = at.z;
        if (fl > 1e-6) cs.forward_x = f.x / fl, cs.forward_z = f.z / fl;
        cs.half_width = vehicle.params().size.x / 2;
        cs.half_length = vehicle.params().size.z / 2;
        cs.speed = vehicle.speed();
        cs.moved = std::fabs(vehicle.speed()) * drive::Vehicle::kStep;
        bool sign_smashed_now = false;
        for (const auto& ev : host.free_roam->step(cs)) {
          char line[160];
          if (ev.kind == freeroam::Event::kSignSmashed) {
            sign_smashed_now = true;
            app.audio_host.trigger_ui(audio::UiCue::kSignSmash);
            audio::CollisionHit bhit{};
            bhit.rel_speed_mps = float(std::fabs(vehicle.speed()));
            bhit.pos = {float(at.x), float(at.y), float(at.z)};
            bhit.is_break = true;
            app.audio_host.trigger_collision(bhit);
            const drive::Vec3 vel_s = vehicle.velocity();
            host.particle_sys.spawn_smashable_burst("flyer", {float(at.x), float(at.y + 0.7), float(at.z)},
                                               {float(vel_s.x), float(vel_s.y + 2.5), float(vel_s.z)});
            const auto& sign_at = app.collectibles.signs[size_t(ev.index)].at;
            app.smashables.spawn_flyer_shatter({float(sign_at.x), float(sign_at.y), float(sign_at.z)},
                                           {float(vel_s.x), float(vel_s.y), float(vel_s.z)});
            const int n = host.free_roam->signs_found();
            std::snprintf(line, sizeof line, "discount sign %d/%zu - upgrades %d percent off", n, app.collectibles.signs.size(),
                          freeroam::upgrade_discount(n));
            std::printf("t %.2f s: %s smashed (%s)\n", vehicle.time(), app.collectibles.signs[size_t(ev.index)].id.c_str(), line);
            popup = {"DISCOUNT SIGN " + std::to_string(n) + "/" + std::to_string(app.collectibles.signs.size()),
                     "UPGRADES " + std::to_string(freeroam::upgrade_discount(n)) + "% OFF", vehicle.time(),
                     double(n) / double(std::max<size_t>(1, app.collectibles.signs.size())), carview::ui::rgba(255, 186, 20)};
          } else if (ev.kind == freeroam::Event::kRumourStarted) {
            std::snprintf(line, sizeof line, "barn find rumour - M for the map");
            popup = {"BARN FIND RUMOUR", "A CLASSIC IS HIDING NEARBY.  M FOR THE MAP", vehicle.time(), -1,
                     carview::ui::rgba(70, 210, 100)};
            app.campaign.on_barn_find(false, app.collectibles.barns[size_t(ev.index)].name, "", &app.audio_host);
            std::printf("t %.2f s: %s rumour at %.1f km driven\n", vehicle.time(),
                        app.collectibles.barns[size_t(ev.index)].name.c_str(), app.progress.total_distance / 1000);
          } else {
            const auto& b = app.collectibles.barns[size_t(ev.index)];
            if (!app.have_garage && app.have_career_catalog) {
              if (const auto* spec = app.career_catalog.find_car_by_media(app.car))
                app.award_garage_car(spec->car_id, 0, true);
            }
            const std::string awarded = app.award_garage_car(b.car_id, 0, false);
            app.garage_picker();
            // The car's name: the Barnfind table's IDS_ key, by the id rule.
            std::string named = barn_car_label(b);
            if (!b.car_name_key.empty()) {
              const uint32_t ref = uint32_t(fh1::string_hash("Barnfind")) << 16 | fh1::string_hash(b.car_name_key);
              const std::string t = carview::game_text("_&" + std::to_string(ref));
              if (!t.empty() && t[0] != '_') named = t;
            }
            if (named == barn_car_label(b) && !awarded.empty()) named = awarded;
            std::snprintf(line, sizeof line, "barn find! %s is in the garage", named.c_str());
            popup = {"BARN FIND!", named + " IS IN YOUR GARAGE", vehicle.time(), -1, carview::ui::rgba(70, 210, 100)};
            app.campaign.on_barn_find(true, b.name, named, &app.audio_host);
            app.pending_barn_restorations.push_back({b.name, named, 45.0, 0.0});
            std::printf("t %.2f s: %s found: %s (Data_Car %d) to the garage\n", vehicle.time(), b.name.c_str(),
                        named.c_str(), b.car_id);
          }
          save_due = true;
        }
        if (host.streamer && host.free_roam->hidden() != host.hidden_sent) host.streamer->set_hidden(host.hidden_sent = host.free_roam->hidden());
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
        // SPEC-CAR-PHYSICS, Aerodynamic drafting in free roam (behind traffic & festival AI).
        if (!app.race) {
          std::vector<drive::DraftCar> leaders;
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
        const drive::Input in = app.race ? app.race->player_input(vehicle, user_in, traffic.get()) : user_in;
        vehicle.step(in, *host.ground);
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
        if (traffic) traffic->step(vehicle);
        if (app.race && host.streamer) {
          app.race->after_step(vehicle, *host.ground, host.streamer->centre_x(), host.streamer->centre_z(),
                           host.streamer->collision_radius(), traffic.get());
          const bool r_started = app.race->session().started();
          if (r_started && !club_race_was_started) club_pending.event_started = true;
          club_race_was_started = r_started;
          const auto& pc = app.race->session().car(app.race->player_index());
          const bool r_finished = (pc.finish >= 0.0);
          if (r_finished && !club_race_was_finished) {
            club_pending.event_completed = true;
            club_pending.event_finish_place = pc.place;
          }
          club_race_was_finished = r_finished;
          if (app.race->is_stage()) {
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
          app.audio_host.trigger_codriver(fin.stage_place <= 10 ? "CoDriver/FinishGood/Good1" : "CoDriver/FinishBad/Bad1");
          if (fin.championship_complete) {
            const int dpct = race::difficulty_bonus_percent(int(app.race_options.difficulty));
            const int apct = race::assist_difficulty_bonus_percent(app.race_options.assists);
            const auto rew = app.rally_session.finalize(app.rally_catalog, app.saved, dpct, apct);
            app.career.credits = app.saved.credits;
            app.career.total_winnings = app.saved.total_winnings;
            app.rally_results_state = carview::make_rally_results_state(app.rally_catalog, app.rally_session, rew, app.saved.credits);
            app.rally_results_open = true;
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
          const int64_t bal_before = app.career.credits;
          const int pop_before = app.current_pop_rank();
          if (const race::RaceAward* a = app.race->award(app.career, app.wristbands, app.race_top_speed, bonus_in)) {
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
            if (app.skill_tracker) app.skill_tracker->reset_race_skills_banked();
            if (app.have_prog_tables) {
              const int drv_xp = slice12.trainer.pacing.scale_points(a->driver_xp);
              const int aff_xp = slice12.trainer.pacing.scale_points(a->affinity_xp);
              const auto pu = race::apply_progression_xp(app.career, drv_xp, current_make_id(), aff_xp,
                                                         app.prog_tables);
              for (int64_t gcid : pu.gift_cars) {
                const std::string gnm = app.award_garage_car(gcid, 0, false);
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
            std::printf("race: award %lld CR (base %lld, mileage %lld, diff +%d%% %lld, assists %lld, skills %lld, nemesis %lld), %d wristband points (total %d, %s), credits %lld%s%s\n",
                        (long long)a->credits, (long long)a->base_credits, (long long)a->mileage_bonus,
                        a->difficulty_pct, (long long)a->difficulty_bonus, (long long)a->assists_bonus,
                        (long long)a->skills_bonus, (long long)a->nemesis_bonus, a->wristband_points,
                        app.career.wristband_points, lvl >= 0 ? app.wristbands.levels[size_t(lvl)].second.c_str() : "no wristband",
                        (long long)app.career.credits, a->first_completion ? ", first completion" : "",
                        a->prize_car ? (", prize car " + std::to_string(a->prize_car)).c_str() : "");
            if (a->prize_car > 0) {
              int prize_col = 0;
              if (const auto* cev = app.career_catalog.find_event(app.race_event)) prize_col = cev->prize_car_colour;
              won_car_name = app.award_garage_car(a->prize_car, prize_col, false);
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
              const int pl = app.race->session().car(app.race->player_index()).place;
              const int ncars = int(app.race->player_index()) + 1;
              app.campaign.on_race_finished(cev, *a, pl, ncars, pop_before, app.current_pop_rank(), bal_before, app.career.credits,
                                        app.career.wristband_points, app.collected_wristband, won_car_name,
                                        app.have_career_catalog ? &app.career_catalog : nullptr, app.fmv_player, &app.audio_host);
              app.campaign_owns_post_race = true;
            }
            save_due = true;  // the window saves the award at once
          }
        }
        const bool smashed_sign = free_roam_step();
        if (smashed_sign) club_pending.smashables.push_back("sign");
        if (host.free_roam && host.free_roam->hidden()) app.smashables.sync_barns_from_hidden(*host.free_roam->hidden());
        bool smashed_prop = smashed_sign;
        // FORMAT-PHYSICS-DEFINITIONS: step 384-body smashable prop pool & barn doors.
        if (!app.bench || app.bench_full_fx) {
          std::vector<world::SmashEvent> sm_events;
          app.smashables.step(vehicle, *host.ground, float(drive::Vehicle::kStep), sm_events);
          for (const auto& sev : sm_events) {
            smashed_prop = true;
            step_object_impact_accum = std::max(step_object_impact_accum, std::max(0.25, std::fabs(vehicle.speed()) / 25.0));
            // Hide intact static world prop immediately on smash and persist across regathers.
            host.showroom.hide_world_props_near(sev.pos, sev.hide_radius);
            if (host.streamer) host.streamer->set_hidden_positions(app.smashables.broken_hidden_positions());
            club_pending.smashables.push_back(
                freeroam::club::classify_smashable_type(sev.prop_stem, sev.object_type_index));
            host.particle_sys.spawn_smashable_burst(sev.prop_stem, sev.pos, sev.impulse_vel);
            audio::CollisionHit bhit{};
            bhit.rel_speed_mps = float(std::fabs(vehicle.speed()));
            bhit.pos = {sev.pos[0], sev.pos[1], sev.pos[2]};
            bhit.is_break = true;
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
          const float half_len = float(std::max(1.4, vehicle.params().size.z * 0.5));
          const float half_w = float(std::max(0.6, vehicle.params().size.x * 0.5));
          const drive::Vec3 ex_l = vpos + drive::rotate(vrot, {-half_w * 0.42, -0.22, -half_len - 0.05});
          const drive::Vec3 ex_r = vpos + drive::rotate(vrot, {half_w * 0.42, -0.22, -half_len - 0.05});
          vin.exhaust_tips[0] = {float(ex_l.x), float(ex_l.y), float(ex_l.z)};
          vin.exhaust_tips[1] = {float(ex_r.x), float(ex_r.y), float(ex_r.z)};
          vin.exhaust_tip_count = 2;

          const float idle_r = float(std::max(600.0, vehicle.params().idle_rpm));
          const float red_r = float(std::max(idle_r + 1000.0, vehicle.params().redline_rpm));
          vin.norm_rpm = std::clamp((float(vehicle.rpm()) - idle_r) / (red_r - idle_r), 0.0f, 1.0f);
          vin.throttle = float(in.throttle);
          vin.upshift_event = (vehicle.gear() > host.last_vfx_gear && vehicle.gear() >= 2);
          host.last_vfx_gear = vehicle.gear();
          vin.backfire_level = std::clamp(audio_upgrades.exhaust, 0, 3);
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
        if (app.skill_tracker && (!app.race || (app.race->session().started() && !app.race->player_finished()))) {
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
          si.car_impact_ms = traffic ? traffic->player_impulse() / mass : 0.0;
          si.smashed_object = smashed_prop;
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
            const int cur_place = app.race->session().car(app.race->player_index()).place;
            if (prev_race_place > 0 && cur_place < prev_race_place && app.race->session().clock() > 2.0) {
              si.passed_opponent = true;
              si.pass_speed_diff_mph = std::max(5.0, si.speed_ms * 0.25);
            }
            prev_race_place = cur_place;
          }
          const int64_t prev_max_skills = app.skill_tracker->max_skills_score();
          for (const auto& sev : app.skill_tracker->step(si)) {
            if (sev.kind == freeroam::SkillEvent::kSkillAwarded) {
              club_pending.skills.push_back(
                  {sev.skill_id, freeroam::club::skill_tracker_grade_to_club_grade(sev.grade)});
            } else if (sev.kind == freeroam::SkillEvent::kChainBanked ||
                       sev.kind == freeroam::SkillEvent::kChainLost) {
              club_pending.skill_chain_completed = true;
            }
            if (sev.kind == freeroam::SkillEvent::kFameSponsor && sev.credits > 0) {
              const int64_t scr = slice12.trainer.pacing.scale_credits(sev.credits);
              app.career.credits = std::min<int64_t>(999999999, app.career.credits + scr);
              app.career.total_winnings += scr;
              app.sponsor_banner_title = "FAME SPONSORSHIP TIER";
              app.sponsor_banner_detail = "RANK " + std::to_string(sev.rank) + "  •  +" + carview::thousands(scr) + " Cr";
              app.sponsor_banner_at = vehicle.time();
              popup = {"FAME SPONSOR BONUS",
                       "POPULARITY RANK " + std::to_string(sev.rank) + "  +" + carview::thousands(scr) + " Cr",
                       vehicle.time(), -1, carview::ui::rgba(247, 255, 38)};
              save_due = true;
            } else if (sev.kind == freeroam::SkillEvent::kSponsorChallenge && sev.credits > 0) {
              const int64_t scr = slice12.trainer.pacing.scale_credits(sev.credits);
              app.career.credits = std::min<int64_t>(999999999, app.career.credits + scr);
              app.career.total_winnings += scr;
              app.sponsor_banner_title = "SPONSORSHIP TIER " + std::to_string(sev.rank) + ": " + sev.skill_id;
              app.sponsor_banner_detail = "+" + carview::thousands(scr) + " Cr REWARD";
              app.sponsor_banner_at = vehicle.time();
              save_due = true;
            } else if (sev.kind == freeroam::SkillEvent::kRankUp) {
              app.campaign.on_popularity_rank_changed(sev.rank + 1, sev.rank, &app.audio_host);
              // Notify if a Showcase event unlocked at this rank!
              if (app.have_career_catalog) {
                for (const auto& cev : app.career_catalog.events)
                  if (cev.style == 3 && cev.popularity_rank_req == sev.rank) {
                    const std::string ename = carview::game_text(cev.name_ref);
                    popup = {"NEW SHOWCASE UNLOCKED!",
                             (!ename.empty() && ename.rfind("_&", 0) != 0 ? ename : cev.horizon_id), vehicle.time(), -1,
                             carview::ui::rgba(245, 4, 92)};
                  }
              }
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
        if (!app.race && app.speed_tracker) {
          const drive::Vec3 sp_at = vehicle.position();
          if (!have_prev_speed_pos) {
            prev_speed_x = sp_at.x;
            prev_speed_z = sp_at.z;
            have_prev_speed_pos = true;
          }
          if (app.demo_speed_trap && !demo_speed_trap_done && vehicle.time() >= 4.0 && !app.speed_data.traps.empty()) {
            demo_speed_trap_done = true;
            const auto& g = app.speed_data.traps.front().gate;
            const double gx = g.right[0] - g.left[0], gz = g.right[2] - g.left[2];
            const double gl = std::max(1.0, std::hypot(gx, gz));
            const double nx = -gz / gl, nz = gx / gl;
            app.speed_tracker->step(drive::Vehicle::kStep, vehicle.time(), g.center[0] - nx * 4.0, g.center[2] - nz * 4.0,
                                g.center[0] + nx * 4.0, g.center[2] + nz * 4.0, 64.5);  // ~144.3 mph flash & PB
          }
          const auto speed_evs = app.speed_tracker->step(drive::Vehicle::kStep, vehicle.time(), prev_speed_x, prev_speed_z,
                                                     sp_at.x, sp_at.z, vehicle.speed());
          prev_speed_x = sp_at.x;
          prev_speed_z = sp_at.z;
          for (const auto& ev : speed_evs) {
            if (ev.kind == freeroam::SpeedActivityEvent::kTrapTriggered ||
                ev.kind == freeroam::SpeedActivityEvent::kZoneCompleted) {
              const bool is_zone = (ev.kind == freeroam::SpeedActivityEvent::kZoneCompleted);
              int sb_id = 0;
              if (is_zone) {
                if (const auto* z = app.speed_data.find_zone(ev.id)) sb_id = z->scoreboard_id;
              } else {
                if (const auto* t = app.speed_data.find_trap(ev.id)) sb_id = t->scoreboard_id;
              }
              club_pending.cameras.push_back({is_zone, sb_id, ev.id, ev.speed_mph});
              if (ev.is_new_pb && app.have_prog_tables) {
                race::apply_progression_xp(app.career, 750, current_make_id(), 350, app.prog_tables);
              }
              if (app.skill_tracker) app.skill_tracker->add_popularity(ev.is_new_pb ? 500.0 : 150.0);
              std::printf("speed activity: %s %.1f mph%s\n", ev.label.c_str(), ev.speed_mph,
                          ev.is_new_pb ? " (NEW PB!)" : "");
              save_due = true;
            }
          }
        }
        // Free-roam Festival AI Racers & instant Head-to-Head point-to-point race.
        if (!app.race && festival_mgr) {
          const drive::Vec3 fp = vehicle.position();
          const drive::Vec3 ff = drive::rotate(vehicle.orientation(), {0, 0, 1});
          const double fh = std::atan2(ff.x, ff.z);
          if (app.demo_h2h && !demo_h2h_started && vehicle.time() >= 3.0) {
            demo_h2h_started = true;
            const uint32_t fid = festival_mgr->spawn_ahead({fp.x, fp.y, fp.z}, fh, 18.0);
            if (fid && festival_mgr->start_head_to_head({fp.x, fp.y, fp.z}, fh, vehicle.time(), fid)) {
              club_pending.encounter_started = true;
            }
            if (app.have_prog_tables && app.career.driver_xp < 36000) {
              race::apply_progression_xp(app.career, 36000, current_make_id(), 18000, app.prog_tables);
            }
            app.sponsor_banner_title = "SPONSORSHIP TIER 2: NEAR MISS";
            app.sponsor_banner_detail = "+10,000 Cr  •  BONDURANT RACING SCHOOL";
            app.sponsor_banner_at = vehicle.time();
          }
          freeroam::HeadToHeadResult hres;
          if (festival_mgr->step(drive::Vehicle::kStep, vehicle.time(), {fp.x, fp.y, fp.z}, fh, vehicle.speed(),
                                 &hres, traffic ? traffic->cars() : nullptr)) {
            club_pending.encounter_completed = true;
            club_pending.encounter_finish_place = hres.won ? 1 : 2;
            ++app.h2h_races;
            if (hres.won) {
              ++app.h2h_wins;
              const int64_t hcr = slice12.trainer.pacing.scale_credits(hres.payout_credits);
              app.career.credits = std::min<int64_t>(999999999, app.career.credits + hcr);
              app.career.total_winnings += hcr;
            }
            if (app.skill_tracker) app.skill_tracker->add_popularity(double(hres.popularity_points));
            if (app.have_prog_tables) {
              const int hxp = slice12.trainer.pacing.scale_points(hres.driver_xp);
              race::apply_progression_xp(app.career, hxp, current_make_id(), hxp / 2, app.prog_tables);
            }
            std::printf("h2h: %s vs %s (+%lld Cr, +%d Fame)\n", hres.won ? "WON" : "LOST",
                        hres.opponent_name.c_str(), (long long)hres.payout_credits, hres.popularity_points);
            save_due = true;
          }
          // Car-to-car physics collision between player and nearby Festival AI / H2H cars.
          {
            const drive::Vec3 fv = vehicle.velocity();
            const auto fcol = festival_mgr->resolve_player_collision(
                {fp.x, fp.y, fp.z}, {fv.x, fv.y, fv.z}, vehicle.params().mass, drive::Vehicle::kStep);
            if (fcol.hit && fcol.impulse_magnitude > 1.0) {
              vehicle.apply_impulse({fcol.contact_point[0], fcol.contact_point[1], fcol.contact_point[2]},
                                    {fcol.player_impulse[0], fcol.player_impulse[1], fcol.player_impulse[2]}, 0.25);
              step_car_impact_accum = std::max(step_car_impact_accum, fcol.impulse_magnitude);
              const double dv_f = fcol.impulse_magnitude / std::max(500.0, vehicle.params().mass);
              if (dv_f > 1.0) {
                audio::CollisionHit hit{};
                hit.rel_speed_mps = float(dv_f);
                hit.pos = {float(fcol.contact_point[0]), float(fcol.contact_point[1]), float(fcol.contact_point[2])};
                hit.is_car_on_car = true;
                app.audio_host.trigger_collision(hit);
              }
            }
          }
        }
        club_step();  // evaluates all active 1000 Club challenges on full step signals
        if (!host.streamer) return true;
        // Headless runs put a car that is stuck or rolled back on the
        // nearest road (the windowed mode has R).
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
        }
        // Off the collision (the .fiz has holes outside the drivable strips):
        // a car 20 m below where its wheels last touched goes back on the
        // road, in the window too (ours; the game keeps cars out with walls).
        {
          bool touching = false;
          for (const auto& w : vehicle.wheels()) touching |= w.contact;
          const drive::Vec3 p = vehicle.position();
          if (touching || !have_touch) last_touch_y = p.y, have_touch = true;
          else if (p.y < last_touch_y - 20) {
            double away = 0;
            world::Spawn sp;
            const bool placed = (safe_reset_ok && safe_reset.place(*host.streamer, vehicle, &away)) ||
                                (world::recover(*host.streamer, vehicle, &sp) && ((away = sp.distance), true));
            std::printf("t %.2f s: fell through the ground at %.1f %.1f %.1f (no collision there)%s\n", vehicle.time(),
                        p.x, p.y, p.z, placed ? ", put back on the road" : "");
            if (placed) {
              rig_accel.reset();
              notice = "off the world: put back on the road";
              notice_until = vehicle.time() + 3;
            }
            last_touch_y = vehicle.position().y;
          }
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
                      vehicle.time(), host.streamer->centre_x(), host.streamer->centre_z(), host.streamer->mesh().triangles(),
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

}  // namespace carview
