#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cars/sources.h"

namespace carview {
namespace {

struct ClubBankedTracker {
  const DriveSession* owner = nullptr;
  double last_banked_at = -1e9;
  double last_sim_time = -1.0;
};

ClubBankedTracker g_club_banked_tracker;

}  // namespace

void DriveSession::club_ensure_meta() {
        if (!*app.car_names_loaded) carview::car_name("");  // ensures v4 carchallenge* string tables are loaded
        if (club_meta_built || !app.have_career_catalog) return;
        club_meta_built = true;
        for (const auto& cev : app.career_catalog.events) {
          std::string nm = carview::game_text(cev.name_ref);
          if (nm.empty() || nm.rfind("_&", 0) == 0) nm = cev.horizon_id;
          club_event_names[cev.event_id] = std::move(nm);
        }
        std::map<uint8_t, std::string> country_names;
        if (app.shared_db) {
          app.shared_db->each(
              "SELECT CountryID, COALESCE(Country, ''), COALESCE(IconPath, '') FROM List_Country", {},
              [&](sqlite3_stmt* st) {
                const int64_t cid = sqlite3_column_int64(st, 0);
                if (cid <= 0 || cid > 255) return;
                const auto* cref = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                const auto* ipath = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                std::string nm = cref ? carview::game_text(cref) : std::string{};
                if ((nm.empty() || nm.rfind("_&", 0) == 0) && ipath && *ipath) nm = ipath;
                if (!nm.empty()) country_names[uint8_t(cid)] = std::move(nm);
              },
              nullptr);
        }
        for (const auto& [cid, spec] : app.career_catalog.cars) {
          freeroam::club::CarMeta m;
          m.car_id = cid;
          m.base_cost = spec.base_cost;
          m.make = app.car_names->short_name(spec.media_name);
          if (auto cit = country_names.find(spec.props.country); cit != country_names.end()) {
            m.country = cit->second;
          }
          club_car_meta[cid] = std::move(m);
        }
}

freeroam::club::ClubMedalStats DriveSession::club_medal_stats() {
        club_ensure_meta();
        std::vector<std::pair<int64_t, int>> per_car;
        per_car.reserve(app.saved.club.size());
        for (const auto& c : app.saved.club) {
          int n = 0;
          for (const auto& ch : c.challenges) n += ch.second ? 1 : 0;
          if (n > 0) per_car.emplace_back(c.car_id, n);
        }
        return freeroam::club::compute_medal_stats(per_car, club_car_meta);
}

drive::SavedClubCar* DriveSession::club_record() {
        for (auto& c : app.saved.club)
          if (c.car_id == club_car) return &c;
        return nullptr;
}

void DriveSession::club_check() {
        club_runs.clear();
        club_car = -1;
        club_gate_kind = ClubGate::kSystem;
        const std::filesystem::path state_path = !app.state_root.empty()
                                                     ? app.state_root
                                                     : (std::getenv("SHIFT_STATE_ROOT")
                                                            ? std::filesystem::path(std::getenv("SHIFT_STATE_ROOT"))
                                                            : std::filesystem::path{});
        if (!app.club_tried) {
          app.club_tried = true;
          app.club_loaded = freeroam::club::load_club(state_path.string(), app.club_data, &app.club_why);
          if (app.club_loaded)
            std::printf("1000 club: %zu cars, %zu challenges (%zu measured), %zu locations\n",
                        app.club_data.cars.size(), app.club_data.challenges(), app.club_data.measured(),
                        app.club_data.locations.size());
        }
        if (!app.club_loaded) return club_gate = app.club_why, void();
        if (!app.colorado_world()) return club_gate = "the 1000 Club is in Colorado", void();
        bool owned = false;
        for (const auto& p : cars::list_packs(state_path))
          if (p.id == app.club_data.package) owned = p.owned();
        if (!owned) return club_gate = "the 1000 Club package is not enabled", void();
        if (app.wristbands.levels.empty() && !app.world_dir.empty()) {
          std::string e;
          race::load_wristband_tables(app.game_root_of(), app.state_root, app.shared_db, app.wristbands, &e);
        }
        if (app.collected_wristband < 1 && !std::getenv("SHIFT_CLUB_ANY_BAND") && !app.demo_club)
          return club_gate_kind = ClubGate::kWristband, club_gate = "needs the green wristband", void();
        // The car driven: the selected garage row when it is this car, else
        // any garage row of this car (a --car pick).
        if (app.have_garage) {
          auto is_this = [&](const garage::Car& gc) {
            auto it = app.baked_ids.find(gc.car_id);
            return it != app.baked_ids.end() && lower_ascii(it->second) == lower_ascii(app.car);
          };
          if (const garage::Car* gc = app.career_garage.selected_car(); gc && is_this(*gc)) club_car = gc->car_id;
          for (const auto& gc : app.career_garage.cars)
            if (club_car < 0 && is_this(gc)) club_car = gc.car_id;
        }
        if (club_car < 0 && app.demo_club && app.have_career_catalog) {
          if (const auto* sp = app.career_catalog.find_car_by_media(app.car)) club_car = sp->car_id;
        }
        if (club_car < 0) return club_gate_kind = ClubGate::kCar, club_gate = "only in your own garage cars", void();
        if (!app.club_data.cars.count(club_car))
          return club_gate_kind = ClubGate::kNoChallenges, club_gate = "this car has no challenges", void();
        club_gate.clear();
        club_gate_kind = ClubGate::kNone;
        const auto& list = app.club_data.cars.at(club_car);
        drive::SavedClubCar* rec = club_record();
        if (!rec) return;  // not begun in this car
        if (rec->challenges.size() < list.size()) rec->challenges.resize(list.size(), {0u, false});
        if (rec->progress.size() < list.size()) rec->progress.resize(list.size());
        for (size_t i = 0; i < list.size(); ++i) {
          if (rec->challenges[i].first == 0u) rec->challenges[i].first = list[i].definition_hash;
          if (!rec->challenges[i].second) {
            freeroam::club::Run r(list[i], app.club_data.locations);
            if (!rec->progress[i].empty()) r.import_save_progress(rec->progress[i]);
            club_runs.push_back({i, std::move(r), 0});
          }
        }
}

void DriveSession::club_step() {
        const double now_t = vehicle.time();
        if (g_club_banked_tracker.owner != this || now_t < g_club_banked_tracker.last_sim_time - 1e-6) {
          g_club_banked_tracker.owner = this;
          g_club_banked_tracker.last_banked_at =
              app.skill_tracker ? app.skill_tracker->hud().banked_at : -1e9;
        }
        g_club_banked_tracker.last_sim_time = now_t;
        if (club_runs.empty()) {
          if (app.skill_tracker) {
            g_club_banked_tracker.last_banked_at =
                std::max(g_club_banked_tracker.last_banked_at, app.skill_tracker->hud().banked_at);
          }
          club_pending.clear();
          return;
        }
        auto& s = club_signal;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 vel = vehicle.velocity();
        const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
        const drive::Vec3 right = drive::rotate(vehicle.orientation(), {1, 0, 0});
        const double v_fwd = vel.x * fwd.x + vel.y * fwd.y + vel.z * fwd.z;
        const double v_lat = std::fabs(vel.x * right.x + vel.y * right.y + vel.z * right.z);
        s.time = vehicle.time();
        s.speed_mph = std::fabs(vehicle.speed()) * 2.2369363;
        s.gear = vehicle.gear() < 0 ? 0 : vehicle.gear() == 0 ? -1 : vehicle.gear();
        bool air = true;
        for (const auto& w : vehicle.wheels()) air = air && !w.contact;
        air = air && vehicle.body_contact_force() <= 0;
        if (air && !club_was_air) club_takeoff = at;
        club_was_air = air;
        s.in_air = air;
        s.is_drifting = !air && s.speed_mph >= 12.0 && v_fwd > 2.0 && v_lat > 2.2 && (v_lat / std::max(1.0, v_fwd)) > 0.18;
        s.jump_m = air ? std::hypot(at.x - club_takeoff.x, at.z - club_takeoff.z) : 0;
        s.collided = vehicle.strongest_body_contact().force > 4 * vehicle.params().mass * 9.81 ||
                     (traffic && traffic->player_impulse() > 2.0 * vehicle.params().mass) ||
                     (app.race && app.race->player_impulse() > 2.0 * vehicle.params().mass);
        s.x = at.x, s.z = at.z;

        // Skills, smashables, speed cameras, events, and H2H pulses.
        s.skills_awarded = std::move(club_pending.skills);
        s.skill_chain_completed = club_pending.skill_chain_completed;
        if (app.skill_tracker) {
          const auto sh = app.skill_tracker->hud();
          s.skill_points = sh.active ? double(freeroam::banked_chain_points(sh.chain_points, sh.multiplier_f)) : 0.0;
          if (sh.banked_at > g_club_banked_tracker.last_banked_at && sh.banked_total > 0) {
            s.banked_skill_points_delta = double(sh.banked_total);
            g_club_banked_tracker.last_banked_at = sh.banked_at;
          } else {
            s.banked_skill_points_delta = 0.0;
          }
        } else {
          s.skill_points = 0.0;
          s.banked_skill_points_delta = 0.0;
        }
        s.smashables_broken = std::move(club_pending.smashables);
        s.speed_cameras = std::move(club_pending.cameras);
        s.event_started = club_pending.event_started;
        s.event_restarted = club_pending.event_restarted;
        s.event_completed = club_pending.event_completed;
        s.event_finish_place = club_pending.event_finish_place;
        s.encounter_started = club_pending.encounter_started;
        s.encounter_completed = club_pending.encounter_completed;
        s.encounter_finish_place = club_pending.encounter_finish_place;
        if (app.race) {
          const auto& ev = app.race->event();
          s.event_id = ev.event_id;
          if (ev.style == 2 || ev.career_type == 4) s.career_event_style = "street";
          else if (ev.style == 1) s.career_event_style = "festival";
          else if (ev.style == 3 || ev.career_type == 7 || ev.career_type == 8) s.career_event_style = "showcase";
          else if (ev.style == 5 || ev.career_type == 11) s.career_event_style = "showdown";
          else if (ev.style == 6 || ev.career_type == 12) s.career_event_style = "finale";
          else s.career_event_style = "none";
          s.event_hub.clear();
          if (app.have_career_catalog) {
            if (const auto* cev = app.career_catalog.find_event(ev.horizon_id); cev && cev->hub_id > 0) {
              std::string hname;
              if (const auto* hub = app.career_catalog.find_hub(cev->hub_id))
                hname = lower_ascii(carview::game_text(hub->name_ref));
              if (hname.find("gladstone") != std::string::npos || cev->hub_id == 2) s.event_hub = "gladstone";
              else if (hname.find("red") != std::string::npos || cev->hub_id == 3) s.event_hub = "redrock";
              else s.event_hub = "carson";
            }
          }
          const auto& pc = app.race->session().car(app.race->player_index());
          const double total_m = std::max(1.0, app.race->racing_line().length() * std::max(1, app.race->session().laps()));
          // Normalized [0..1] race progress; stays < 0 before rolling off start line so wait-at-start works.
          const double frac = std::clamp(pc.distance / total_m, 0.0, 1.0);
          const double raw_prog = app.race->session().started() ? (pc.distance > 0.5 ? frac : -0.01) : -0.01;
          s.race_progress = (raw_prog < 0.0 || (std::fabs(vehicle.speed()) < 1.5 && pc.distance < 1.0)) ? -0.01 : raw_prog;
        } else {
          s.event_id = 0;
          s.event_hub.clear();
          s.career_event_style = "none";
          s.race_progress = -1.0;
        }
        club_pending.clear();

        club_ensure_meta();
        const fh1::StringTables* stbls = app.car_names->strings();
        const auto& list = app.club_data.cars.at(club_car);
        drive::SavedClubCar* rec = club_record();
        for (size_t k = 0; k < club_runs.size();) {
          ClubRun& r = club_runs[k];
          const std::string title = list[r.index].format_title(stbls);
          const std::string desc = list[r.index].format_description(stbls, &club_event_names);
          const bool won = r.run.step(s);
          // Sync save_progress partial counts into the save record whenever they advance.
          if (rec && r.run.has_save_progress() && r.index < rec->progress.size()) {
            auto cur_prog = r.run.export_save_progress();
            if (cur_prog != rec->progress[r.index]) {
              rec->progress[r.index] = std::move(cur_prog);
              save_due = true;
            }
          }
          if (won) {
            if (rec) {
              rec->challenges[r.index].first =
                  rec->challenges[r.index].first ? rec->challenges[r.index].first : list[r.index].definition_hash;
              rec->challenges[r.index].second = true;
            }
            int done = 0;
            if (rec)
              for (const auto& c : rec->challenges) done += c.second;
            const auto mstats = club_medal_stats();
            std::printf("t %.2f s: 1000 club medal! %s (%d/%zu in this car, %d total)\n", s.time, title.c_str(), done,
                        list.size(), mstats.total_medals);
            club_ceremony = {};
            club_ceremony.active = true;
            club_ceremony.challenge_title = title;
            club_ceremony.challenge_desc = desc;
            club_ceremony.car_name = carview::car_name(app.car);
            club_ceremony.car_medals = done;
            club_ceremony.car_total = int(list.size());
            club_ceremony.total_medals = mstats.total_medals;
            club_ceremony.cars_with_medal = mstats.cars_with_medal;
            club_ceremony.age = 0.0;
            club_ceremony_at = s.time;
            club_results.push_back({r.index, true, s.time});
            // interim (SPEC_QUESTIONS Q144): ui4audio.xml names no medal
            // play; the HUD notification's cue stands in (not the
            // sign smash's).
            app.audio_host.trigger_ui(audio::UiCue::kAchievementReward);
            save_due = true;
            club_runs.erase(club_runs.begin() + std::ptrdiff_t(k));
            continue;
          }
          if (r.run.failures() != r.failures_seen) {
            r.failures_seen = r.run.failures();
            club_results.push_back({r.index, false, s.time});
            std::printf("t %.2f s: 1000 club: %s failed\n", s.time, title.c_str());
          }
          ++k;
        }
}

ClubScreenState DriveSession::build_club_screen() {
        club_ensure_meta();
        const fh1::StringTables* stbls = app.car_names->strings();
        carview::ClubScreenState cs;
        cs.active = true;
        cs.car_name = carview::car_name(app.car);
        cs.stats = club_medal_stats();
        if (!club_gate.empty() || club_car < 0 || !app.club_data.cars.count(club_car)) {
          cs.gate_reason = club_gate.empty() ? "1000 Club is unavailable for this car" : club_gate;
          return cs;
        }
        const auto& list = app.club_data.cars.at(club_car);
        const drive::SavedClubCar* rec = club_record();
        cs.challenges_begun = (rec != nullptr);
        cs.show_intro = (rec == nullptr || cs.stats.total_medals == 0);
        cs.car_total = int(list.size());
        cs.selected = int(std::min(club_pick, list.empty() ? size_t(0) : list.size() - 1));
        for (size_t i = 0; i < list.size(); ++i) {
          carview::ClubChallengeRow row;
          row.title = list[i].format_title(stbls);
          row.description = list[i].format_description(stbls, &club_event_names);
          row.hint = list[i].format_hint(0, stbls);
          row.completed = (rec && i < rec->challenges.size() && rec->challenges[i].second);
          if (row.completed) {
            ++cs.car_medals;
            row.progress = 1.0;
            row.status = "MEDAL EARNED";
          } else if (rec) {
            for (const auto& r : club_runs) {
              if (r.index == i) {
                row.status = r.run.status(club_signal);
                row.progress = r.run.progress(club_signal);
                row.has_save_progress = r.run.has_save_progress();
                break;
              }
            }
          } else {
            row.status = "READY";
          }
          cs.rows.push_back(std::move(row));
        }
        return cs;
}

void DriveSession::club_page(std::string& title, std::vector<std::string>& lines) {
        lines.clear();
        const auto cs = build_club_screen();
        if (!cs.gate_reason.empty()) {
          title = "1000 club";
          lines.push_back(cs.gate_reason);
          return;
        }
        title = "1000 club: " + std::to_string(cs.car_medals) + "/" + std::to_string(cs.car_total) +
                " medals in this car";
        for (const auto& r : cs.rows) {
          lines.push_back(r.title + " - " + r.description + " [" + r.status + "]");
        }
        lines.push_back(cs.challenges_begun ? "challenges running in this car" : "begin challenges in this car");
}

ClubActiveStrip DriveSession::build_club_active_strip() {
        carview::ClubActiveStrip st;
        if (club_runs.empty()) return st;
        club_ensure_meta();
        const fh1::StringTables* stbls = app.car_names->strings();
        const auto& list = app.club_data.cars.at(club_car);
        for (const auto& r : club_runs) {
          if (r.run.under_way(club_signal)) {
            st.active = true;
            st.title = list[r.index].format_title(stbls);
            st.status = r.run.status(club_signal);
            st.progress = r.run.progress(club_signal);
            return st;
          }
        }
        return st;
}

std::string DriveSession::club_banner() {
        if (club_v4_hud_shown) return {};
        const auto s = build_club_active_strip();
        return s.active ? (text_of(club_text::kTitle) + ": " + s.title + " - " + s.status) : std::string();
}

ClubMenuData DriveSession::build_club_menu_data() {
        ClubMenuData d;
        club_ensure_meta();
        d.tutorial = app.saved.club_tutorial;
        d.stats = club_medal_stats();
        d.achievements = club_achievement_lines(d.stats);
        for (auto& a : d.achievements) {
          if (const char* n = achievement_fallback_name(a.id)) a.name = n;
          a.gamerscore = achievement_fallback_gamerscore(a.id);
        }
        if (club_gate_kind == ClubGate::kWristband) {
          d.gate_title = club_text::kUnavailableTitle;
          d.gate_message = club_text::kUnavailableMsg;
          return d;
        }
        if (club_gate_kind == ClubGate::kSystem) {
          // No string table has the engine's reasons (no data, not in
          // Colorado, the package off).
          d.gate_title = club_text::kUnavailableTitle;
          d.gate_message = StringRef{{}, {}, 0, club_gate, true};
          return d;
        }
        const fh1::StringTables* stbls = app.car_names->strings();
        const auto& defs = freeroam::club::club_achievements();
        auto add = [&](int64_t car_id, const garage::Car* gc) {
          const auto cit = app.club_data.cars.find(car_id);
          if (cit == app.club_data.cars.end()) return;
          ClubCarEntry e;
          e.car_id = car_id;
          e.garage_id = gc ? gc->id : 0;
          const auto bit = app.baked_ids.find(car_id);
          const std::string media = bit != app.baked_ids.end() ? bit->second : std::string();
          e.name = media.empty() ? std::to_string(car_id) : app.car_names->model_name(media);
          e.year_make = media.empty() ? std::string() : app.car_names->year_make(media);
          if (gc && !app.class_bands.empty()) {
            e.class_letter = app.class_bands[size_t(garage::class_of(gc->performance_index, app.class_bands))].name;
            e.pi = garage::shown_index(gc->performance_index, app.class_bands);
          }
          const drive::SavedClubCar* rec = nullptr;
          for (const auto& c : app.saved.club)
            if (c.car_id == car_id) rec = &c;
          for (size_t i = 0; i < cit->second.size(); ++i) {
            e.challenge_titles.push_back(cit->second[i].format_title(stbls));
            e.completed.push_back(rec && i < rec->challenges.size() && rec->challenges[i].second);
          }
          e.medals = freeroam::club::car_medals(app.club_data, car_id, rec ? &e.completed : nullptr);
          if (auto mit = club_car_meta.find(car_id); mit != club_car_meta.end())
            for (size_t i = 0; i < defs.size(); ++i) e.counts[i] = freeroam::club::counts_for(defs[i].id, mit->second);
          e.current = car_id == club_car;
          d.garage.push_back(std::move(e));
        };
        std::set<int64_t> seen;
        if (app.have_garage) {
          for (const auto& gc : app.career_garage.cars)
            if (seen.insert(gc.car_id).second) add(gc.car_id, &gc);
        }
        if (club_car >= 0 && !seen.count(club_car)) add(club_car, nullptr);  // --demo-club's car
        for (size_t i = 0; i < d.garage.size(); ++i)
          if (d.garage[i].current) d.current = int(i);
        if (club_car >= 0 && club_record()) {
          d.current_begun = true;
          for (auto& row : build_club_screen().rows) {
            ClubChallengeLine c;
            c.title = std::move(row.title);
            c.description = std::move(row.description);
            c.hint = std::move(row.hint);
            c.status = std::move(row.status);
            c.completed = row.completed;
            c.progress = row.progress;
            d.challenges.push_back(std::move(c));
          }
        }
        return d;
}

ClubHud DriveSession::build_club_hud() {
        ClubHud hud;
        const double now = vehicle.time();
        club_results.erase(std::remove_if(club_results.begin(), club_results.end(),
                                          [&](const ClubResult& r) { return now - r.at > kClubResultHold || now < r.at; }),
                           club_results.end());
        if (club_car < 0 || !app.club_data.cars.count(club_car) || !club_record()) return hud;
        club_ensure_meta();
        const fh1::StringTables* stbls = app.car_names->strings();
        const auto& list = app.club_data.cars.at(club_car);
        for (const auto& r : club_results) {
          if (hud.items.size() >= 6 || r.index >= list.size()) break;
          ClubHudItem it;
          it.number = int(r.index) + 1;
          it.text = list[r.index].format_title(stbls);
          it.result = r.passed ? ClubHudItem::Result::kPassed : ClubHudItem::Result::kFailed;
          it.result_age = now - r.at;
          hud.items.push_back(std::move(it));
        }
        for (const auto& r : club_runs) {
          if (hud.items.size() >= 6) break;
          if (!r.run.under_way(club_signal)) continue;
          bool shown = false;
          for (const auto& it : hud.items) shown = shown || it.number == int(r.index) + 1;
          if (shown) continue;
          ClubHudItem it;
          it.number = int(r.index) + 1;
          it.text = list[r.index].format_title(stbls) + "  " + r.run.status(club_signal);
          it.progress = r.run.progress(club_signal);
          hud.items.push_back(std::move(it));
        }
        // club_begin.png: the details prompt in free roam once challenges are
        // begun, and the tutorial's next step at the top left.
        hud.details_hint = !app.race;
        if (app.saved.club_tutorial == 2) hud.instruction = text_of(club_text::kTutorialGoToDetails);
        // interim (SPEC_QUESTIONS Q143): after the first medal the tutorial
        // sends the player back to the 1000 CLUB screen.
        else if (app.saved.club_tutorial == 3 && club_medal_stats().total_medals > 0)
          hud.instruction = text_of(club_text::kTutorialGoToHome);
        return hud;
}

void DriveSession::club_open_menu() {
        club_check();
        club_open = true;
        club_pick = 0;
        club_menu_data = build_club_menu_data();
        // Without the title update's scenes the hand-drawn screen stays.
        if (!SceneScreen(kClubHomeScene).ok()) {
          club_menu.close();
          return;
        }
        // The tutorial's last step: back on the 1000 CLUB screen after the
        // first medal.
        if (app.saved.club_tutorial == 3 && club_menu_data.stats.total_medals > 0) {
          app.saved.club_tutorial = 4;
          club_menu_data.tutorial = 4;
          save_now("1000 club");
        }
        club_menu.show(club_menu_data);
        app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
        if (club_menu.screen == ClubMenu::Screen::kIntro) {
          club_menu.opened_at = vehicle.time();
          const auto mp = FmvPlayer::resolve_movie(app.game_root_of(), FmvMovieId::kClubIntro, app.state_root);
          if (!mp.empty() && app.fmv_player.open(mp, app.language)) {
            app.audio_host.set_fmv_audio(
                [](void* ctx, float* out, int fr) { return static_cast<FmvPlayer*>(ctx)->read_audio(out, fr); },
                &app.fmv_player);
          }
        }
}

void DriveSession::club_menu_press(ClubMenu::Button b) {
        const ClubMenu::Action a = club_menu.press(b, club_menu_data);
        switch (a) {
          case ClubMenu::Action::kNone: break;
          case ClubMenu::Action::kMove: app.audio_host.trigger_ui(audio::UiCue::kMove); break;
          case ClubMenu::Action::kAccept:
          case ClubMenu::Action::kTutorialStep: app.audio_host.trigger_ui(audio::UiCue::kAccept); break;
          case ClubMenu::Action::kBack:
          case ClubMenu::Action::kClose: app.audio_host.trigger_ui(audio::UiCue::kCancel); break;
          case ClubMenu::Action::kDeny: app.audio_host.trigger_ui(audio::UiCue::kError); break;
          case ClubMenu::Action::kBegin:
            app.audio_host.trigger_ui(audio::UiCue::kAccept);
            club_begin_in(club_menu.action_car);
            break;
        }
        if (club_menu.tutorial_to > app.saved.club_tutorial) {
          app.saved.club_tutorial = club_menu.tutorial_to;
          club_menu_data.tutorial = club_menu.tutorial_to;
          save_now("1000 club");
        }
        club_menu.tutorial_to = 0;
        if (!club_menu.open()) {
          club_open = false;
          if (app.fmv_player.active()) app.fmv_player.close();
        }
}

void DriveSession::club_begin_in(int64_t car_id) {
        const auto cit = app.club_data.cars.find(car_id);
        if (cit == app.club_data.cars.end()) return;
        bool have = false;
        for (const auto& c : app.saved.club) have = have || c.car_id == car_id;
        if (!have) {
          drive::SavedClubCar rec;
          rec.car_id = car_id;
          for (const auto& ch : cit->second) {
            rec.challenges.emplace_back(ch.definition_hash, false);
            rec.progress.emplace_back();
          }
          app.saved.club.push_back(std::move(rec));
        }
        // Another garage car: into it (the garage's selection and the drive's
        // car), as the garage's GET IN CAR does.
        if (car_id != club_car && app.have_garage) {
          for (const auto& gc : app.career_garage.cars) {
            if (gc.car_id != car_id) continue;
            app.career_garage.selected = gc.id;
            app.write_career_garage();
            break;
          }
          if (auto bit = app.baked_ids.find(car_id); bit != app.baked_ids.end() && bit->second != app.car) {
            save_now("1000 club");
            app.next_car = bit->second;
            running = false;
            return;
          }
        }
        save_now("1000 club");
        club_check();
}


void DriveSession::rally_entry_press(bool accept) {
        const RallyEntryPopup p = rally_entry;
        rally_entry = RallyEntryPopup::kNone;
        if (p == RallyEntryPopup::kGate || !accept) {
          app.audio_host.trigger_ui(audio::UiCue::kCancel);
          return;
        }
        app.audio_host.trigger_ui(audio::UiCue::kAccept);
        if (p == RallyEntryPopup::kQuit) {
          // The original unloads to the title (CUnloadToPreGame splash);
          // the engine goes back to the festival instead (Q211).
          app.rally_hub_open = false;
          app.rally_hub_track_layout = false;
          app.rally_world_switch = -1;
          running = false;
          return;
        }
        // The first entry plays Forza_Tone_Rally before the switch; the
        // switch then runs the intro race, the first Autoshow and the hub's
        // tutorial (DriveSave::rally_flow; Q145).
        if (app.saved.rally_flow == 0) {
          app.saved.rally_flow = 1;
          save_now("horizon rally");
          const auto mp = FmvPlayer::resolve_movie(app.game_root_of(), FmvMovieId::kRallyForzaTone, app.state_root);
          if (!mp.empty() && app.fmv_player.open(mp, app.language)) {
            app.audio_host.set_fmv_audio(
                [](void* ctx, float* out, int fr) { return static_cast<FmvPlayer*>(ctx)->read_audio(out, fr); },
                &app.fmv_player);
            rally_enter_after_video = true;
            return;
          }
        }
        rally_enter();
}

void DriveSession::rally_enter() {
        const drive::Vec3 vp = vehicle.position();
        const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
        app.colorado_return_pos = {vp.x, vp.y, vp.z};
        app.colorado_return_heading = std::atan2(vf.x, vf.z);
        app.has_colorado_return = true;
        app.rally_world_switch = +1;
        running = false;
}

}  // namespace carview
