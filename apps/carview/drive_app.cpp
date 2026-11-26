#include "drive_app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "cars/mods.h"
#include "drive/cameras.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "drive_helpers.h"
#include "frontend_screens.h"
#include "gamepad_db.h"
#include "freeroam/tunables.h"
#include "garage/rally_kit.h"
#include "platform_support.h"
#include "profile/drive_save.h"
#include "race/difficulty.h"
#include "settings.h"
#include "showroom.h"
#include "slice12_runtime.h"

namespace carview {
namespace {

template <typename EvT>
bool extract_event_tod_start(const EvT& ev, double& out_tod_sec) {
  if constexpr (requires { ev.has_tod_start; ev.tod_start_sec; }) {
    if (ev.has_tod_start && ev.tod_start_sec >= 0.0) {
      out_tod_sec = double(ev.tod_start_sec);
      return true;
    }
  }
  return false;
}

}  // namespace

bool DriveApp::colorado_world() const { return world_dir.empty() || world::model_prefix(world_dir) == "coloradoout"; }

std::filesystem::path DriveApp::game_root_of() const {
    return !game_root_given.empty() ? game_root_given : world_dir.parent_path().parent_path().parent_path();
}

void DriveApp::write_career_garage() {
    std::string e;
    if (have_garage && !garage_path.empty() && !garage::write_garage(garage_path, career_garage, &e))
      std::fprintf(stderr, "carview: %s\n", e.c_str());
}

// WORLD-11: an event with a start time sets the clock there, and free roam
// carries on from the event's time afterwards. The original does not put
// the pre-race time back: quitting an event entered from a 22:00 free roam
// returns to free roam in daylight (Q107).
void DriveApp::snap_to_event_tod(const std::string& id) {
    if (!race) return;
    double event_tod_sec = -1.0;
    bool has_event_tod = extract_event_tod_start(race->event(), event_tod_sec);
    if (!has_event_tod && have_career_catalog) {
      if (const auto* cev = career_catalog.find_event(id)) {
        has_event_tod = extract_event_tod_start(*cev, event_tod_sec);
      }
    }
    if (has_event_tod && event_tod_sec >= 0.0) {
      double sec = std::fmod(event_tod_sec, 86400.0);
      if (sec < 0.0) sec += 86400.0;
      tod_clock.tod_sec = sec;
    }
}
bool DriveApp::start_race(const std::string& id, std::string& why) {
    if (!race_pi_cfg_loaded && !world_dir.empty()) {
      race_pi_cfg_loaded = true;
      const auto pzip = cars::mods::resolve(game_root_of() / "media" / "physics.zip");
      std::error_code pec;
      if (std::filesystem::exists(pzip, pec)) {
        cars::load_physics_settings(pzip, race_phys_settings, nullptr);
        garage::load_pi_config(pzip, race_pi_cfg, nullptr);
      }
    }
    const std::string saved_car_before = car;
    const std::string saved_pre_loaner = pre_loaner_car;
    const int64_t saved_pre_loaner_gid = pre_loaner_garage_id;
    if (!pending_event_loaner_car.empty()) {
      const std::string loaner = pending_event_loaner_car;
      pending_event_loaner_car.clear();
      if (pre_loaner_car.empty()) {
        pre_loaner_car = car;
        pre_loaner_garage_id = career_garage.selected;
      }
      car = loaner;
    }
    race_options.autopilot = race_auto < 0 ? headless : race_auto == 1;
    race_options.player_car = car;
    race_options.player_pi = carview::resolve_player_race_pi(
        have_garage ? career_garage.selected_car() : nullptr, car, baked_ids, shared_db, &race_phys_settings,
        &race_pi_cfg, &class_bands);
    race_options.stage_world = world_dir;  // a Rally stage's route and line (--event stage:<route>)
    race_options.state_root = state_root;
    race_options.parts_db = shared_db;
    race_options.language = language;
    auto r = std::make_unique<carview::RaceView>(bake_dir);
    if (!r->open(game_root_of(), id, race_options, why)) {
      car = saved_car_before;
      pre_loaner_car = saved_pre_loaner;
      pre_loaner_garage_id = saved_pre_loaner_gid;
      return false;
    }
    race = std::move(r);
    race->player_spawn(spawn);
    have_spawn = true;
    race_top_speed = 0;
    snap_to_event_tod(id);

    if (wristbands.levels.empty()) {
      std::string e;
      if (!race::load_wristband_tables(game_root_of(), state_root, shared_db, wristbands, &e))
        std::fprintf(stderr, "carview: note: no wristband tables: %s\n", e.c_str());
    }
    if (!have_post_race_options) {
      std::string e;
      have_post_race_options = race::load_post_race_options(game_root_of(), post_race_options, &e);
      if (!have_post_race_options)
        std::fprintf(stderr, "carview: note: no PostRaceOptions, no mileage bonus: %s\n", e.c_str());
    }
    return true;
}

int DriveApp::current_pop_rank() const { return skill_tracker ? skill_tracker->popularity_rank() : 250; }

std::string DriveApp::loading_group() const {
  if (!race) return "freeroam";
  const race::CareerEvent* ev = have_career_catalog ? career_catalog.find_event(race_event) : nullptr;
  return ev ? loading_group_for_event(ev->style, ev->level) : std::string("festivalevent");
}

std::vector<std::string> DriveApp::class_name_list() const {
    std::vector<std::string> out;
    for (const auto& b : class_bands) out.push_back(b.name);
    return out;
}

std::string DriveApp::award_garage_car(int64_t car_id, int colour_seq, bool select_it, bool is_prize,
                                       int upgrade_class) {
    const auto* spec = have_career_catalog ? career_catalog.find_car(car_id) : nullptr;
    std::string media = spec ? spec->media_name : std::string();
    if (media.empty()) {
      auto it = baked_ids.find(car_id);
      if (it != baked_ids.end()) media = it->second;
    }
    if (car_id <= 0 || media.empty()) return {};
    // Ensure garage is enabled.
    have_garage = true;
    saved.owned_garage = true;
    // Check if we already own this exact prize car (when not forced starter).
    if (!select_it) {
      for (const auto& gc : career_garage.cars)
        if (gc.car_id == car_id) return carview::car_name(media);
    }
    std::map<std::string, int64_t> stock;
    if (parts_db.valid()) {
      if (const auto db = parts_db.get()) {
        std::string ignored;
        db->stock_slots(car_id, stock, &ignored);
      }
    }
    garage::Car gc;
    gc.id = ++career_garage.last_id;
    gc.car_id = car_id;
    gc.performance_index = spec ? spec->stock_pi : 0.5;
    gc.class_id = spec ? spec->stock_class : (class_bands.empty() ? 2 : garage::class_of(gc.performance_index, class_bands));
    gc.parts_value = 0;
    gc.colour = colour_seq;
    gc.is_prize = is_prize;
    gc.slots = std::move(stock);
    career_garage.cars.push_back(std::move(gc));
    if (select_it || career_garage.selected <= 0) career_garage.selected = career_garage.cars.back().id;
    // A prize car comes fitted to its upgrade class, free of charge (the
    // parts the quick upgrade would pick, paid from a matching grant).
    if (upgrade_class >= 0 && upgrade_class > career_garage.cars.back().class_id && parts_db.valid() &&
        !class_bands.empty()) {
      if (!race_pi_cfg_loaded && !world_dir.empty()) {
        race_pi_cfg_loaded = true;
        const auto pzip = cars::mods::resolve(game_root_of() / "media" / "physics.zip");
        std::error_code pec;
        if (std::filesystem::exists(pzip, pec)) {
          cars::load_physics_settings(pzip, race_phys_settings, nullptr);
          garage::load_pi_config(pzip, race_pi_cfg, nullptr);
        }
      }
      if (const auto db = parts_db.get()) {
        const int64_t gid = career_garage.cars.back().id;
        const auto plan = garage::plan_auto_upgrade(career_garage, *db, gid, upgrade_class, 0, race_phys_settings,
                                                    race_pi_cfg, class_bands);
        int64_t free_credits = plan.total_cost;
        std::string why;
        if (!plan.ok || !garage::apply_auto_upgrade(career_garage, *db, gid, plan, 0, free_credits,
                                                    race_phys_settings, race_pi_cfg, class_bands, &why))
          std::fprintf(stderr, "carview: prize car %lld not upgraded to class %d: %s\n", (long long)car_id,
                       upgrade_class, (why.empty() ? plan.message : why).c_str());
      }
    }
    write_career_garage();
    note_car_obtained(car_id, false);
    return carview::car_name(media);
}

bool DriveApp::ensure_hero_car() {
    // The registry is read once (this runs with every car load in the opening).
    static const freeroam::HeroCarPreset hero = [&] {
      freeroam::Registry reg;
      std::string why;
      if (!world_dir.empty() && freeroam::load_registry(game_root_of(), reg, &why))
        return freeroam::HeroCarPreset::from(reg);
      return freeroam::HeroCarPreset::disc();
    }();
    for (const auto& gc : career_garage.cars)
      if (gc.car_id == hero.car_id) {
        if (career_garage.selected != gc.id) {
          career_garage.selected = gc.id;
          write_career_garage();
        }
        return true;
      }
    if (award_garage_car(hero.car_id, hero.colour_index, true).empty()) return false;
    const auto db = parts_db.valid() ? parts_db.get() : nullptr;
    if (!db) return true;
    if (!race_pi_cfg_loaded && !world_dir.empty()) {
      race_pi_cfg_loaded = true;
      const auto pzip = cars::mods::resolve(game_root_of() / "media" / "physics.zip");
      std::error_code pec;
      if (std::filesystem::exists(pzip, pec)) {
        cars::load_physics_settings(pzip, race_phys_settings, nullptr);
        garage::load_pi_config(pzip, race_pi_cfg, nullptr);
      }
    }
    std::string why;
    const int64_t gid = career_garage.cars.back().id;
    if (!garage::fit_upgrade_levels(career_garage, *db, gid, hero.upgrades, hero.wheel_style, &race_phys_settings,
                                    &race_pi_cfg, class_bands.empty() ? nullptr : &class_bands, &why))
      std::fprintf(stderr, "carview: hero car %lld: %s\n", (long long)hero.car_id, why.c_str());
    if (const garage::Car* gc = career_garage.find(gid))
      std::printf("hero car: %lld fitted, PI %.4f class %d\n", (long long)hero.car_id, gc->performance_index,
                  gc->class_id);
    write_career_garage();
    return true;
}

std::string DriveApp::start_rally_heats() {
  if (!rally_catalog.loaded ||
      !rally_session.start_heats(rally_catalog, int(race_options.difficulty), race_options.seed))
    return {};
  if (auto bit = baked_ids.find(rally_catalog.heats.player_car_id); bit != baked_ids.end()) {
    pre_loaner_car = car;
    pre_loaner_garage_id = career_garage.selected;
    car = bit->second;
  }
  rally_hub_open = false;
  race_options.stage_title = rally_catalog.heats.name;
  return "stage:" + std::to_string(rally_catalog.heats.route_id);
}

void DriveApp::note_car_obtained(int64_t car_id, bool purchased) {
  if (car_id <= 0 || !parts_db.valid()) return;
  const auto db = parts_db.get();
  if (db && garage::is_horizon_rally_car(*db, car_id)) drive::note_rally_car(saved.ach, car_id, purchased);
}

CareerSummary DriveApp::career_summary(std::vector<cars::PackInfo>& packs) {
    carview::CareerSummary cs;
    if (wristbands.levels.empty() && !world_dir.empty()) {
      std::string e;
      race::load_wristband_tables(game_root_of(), state_root, shared_db, wristbands, &e);
    }
    const int li = wristbands.level_index(career.wristband_points);
    if (li >= 0) cs.level_name = carview::game_text(wristbands.levels[size_t(li)].second);
    // A name that is a string-table key (the tables are not read: interim):
    // the level's number instead.
    if (cs.level_name.rfind("_&", 0) == 0) cs.level_name = std::to_string(li);
    cs.level = li;
    cs.signs_found = int(saved.signs_found.size());
    // Every barn on the disc (barnfinds.xml), as the free roam counts them;
    // the save lists only the barns it has a state for.
    cs.barns = int(std::max(saved.barns.size(), collectibles.barns.size()));
    for (const auto& b : saved.barns) cs.barns_found += freeroam::barn_discovered(b.status);
    cs.wristband_points = career.wristband_points, cs.credits = career.credits;
    cs.total_winnings = career.total_winnings, cs.races = career.num_races;
    cs.victories = career.num_victories, cs.podiums = career.num_podiums;
    cs.garage_cars = int(career_garage.cars.size());
    cs.club_cars = int(saved.club.size());
    for (const auto& c : saved.club)
      for (const auto& [key, done] : c.challenges) cs.club_total++, cs.club_done += done;
    cs.has_rally = saved.has_rally, cs.rallies = saved.num_rallies, cs.rally_stages = saved.num_rally_stages;
    cs.rally_victories = saved.num_rally_victories, cs.rally_winnings = saved.rally_winnings;
    cs.rally_xp = saved.rally_xp, cs.rally_level = saved.rally_level;
    // The exploration counts from the same trackers the board's right column
    // reads (VIS-24: the summary left them at 0 while the board showed the
    // live counts).
    cs.popularity_rank = current_pop_rank();
    if (road_discovery.ready()) {
      cs.roads_discovered = road_discovery.discovered_count();
      cs.roads_total = road_discovery.total_roads();
    }
    if (have_outposts) {
      cs.outposts_discovered = outpost_progress.discovered_count(outposts);
      cs.outposts_total = int(outposts.outposts.size());
      cs.pr_stunts_done = outpost_progress.completed_stunt_count(outposts);
      cs.pr_stunts_total = int(outposts.stunts.size());
    }
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    packs = cars::list_packs(state ? state : "");
    // SPEC-V4-CONTENT, Progression: Rally needs its package (offer 1600).
    cs.rally_owned = std::any_of(packs.begin(), packs.end(),
                                 [](const cars::PackInfo& p) { return p.puboffer == 1600 && p.owned(); });
    return cs;
}

CareerBoard DriveApp::career_board(const CareerPage& page) {
    const carview::CareerSummary& cs = page.summary();
    carview::CareerBoard b;
    if (!cs.level_name.empty()) b.band_name = cs.level_name + " WRISTBAND";
    b.points = cs.wristband_points;
    const int li = wristbands.levels.empty() ? -1 : wristbands.level_index(cs.wristband_points);
    b.band_lo = li >= 0 ? wristbands.levels[size_t(li)].first : 0;
    if (size_t(li + 1) < wristbands.levels.size()) {
      b.band_hi = wristbands.levels[size_t(li + 1)].first;
      b.band_next = carview::game_text(wristbands.levels[size_t(li + 1)].second);
      if (b.band_next.rfind("_&", 0) == 0) b.band_next.clear();
    }
    b.credits = cs.credits, b.winnings = cs.total_winnings;
    b.races = cs.races, b.wins = cs.victories, b.podiums = cs.podiums, b.garage = cs.garage_cars;
    b.popularity_rank = current_pop_rank();
    b.max_skills = skill_tracker ? skill_tracker->max_skills_score() : 0;
    b.driver_level = std::max(1, career.driver_level);
    b.driver_xp = career.driver_xp;
    b.speed_traps = speed_tracker ? int(speed_tracker->traps_completed()) : int(saved.speed_traps.size());
    b.speed_traps_total = int(speed_data.traps.size());
    b.speed_zones = speed_tracker ? int(speed_tracker->zones_completed()) : int(saved.speed_zones.size());
    b.speed_zones_total = int(speed_data.zones.size());
    b.h2h_wins = h2h_wins;
    b.h2h_races = h2h_races;
    b.signs = cs.signs_found, b.barns = cs.barns_found, b.barns_total = cs.barns;
    b.club_cars = cs.club_cars, b.club_done = cs.club_done, b.club_total = cs.club_total;
    b.rally = cs.has_rally, b.rally_level = cs.rally_level, b.rally_xp = cs.rally_xp;
    b.rallies = cs.rallies, b.rally_wins = cs.rally_victories;
    b.packs_owned = page.packs_owned(), b.packs_total = page.packs_total();
    return b;
}

size_t DriveApp::garage_picker() {
    std::vector<const garage::Car*> rows;
    for (const auto& c : career_garage.cars)
      if (baked_ids.count(c.car_id)) rows.push_back(&c);
    std::stable_sort(rows.begin(), rows.end(), [](const garage::Car* a, const garage::Car* b) {
      return a->class_id != b->class_id ? a->class_id < b->class_id : a->performance_index < b->performance_index;
    });
    std::vector<std::string> keep;
    std::vector<int64_t> ids;
    for (const auto* c : rows) keep.push_back(lower_ascii(baked_ids.at(c->car_id))), ids.push_back(c->id);
    if (!keep.empty()) baked_cars = std::move(keep), pick_ids = std::move(ids);
    if (sync_picker) sync_picker();
    return rows.size();
}

bool DriveApp::init(int& exit_code) {
  exit_code = 0;
  if (!record_session_dir.empty()) std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (!autoplay_script.empty()) {
    std::string ap_err;
    const auto ap_out = autoplay_out.empty() ? autoplay_script.parent_path() / "autoplay" : autoplay_out;
    if (!autoplay.load(autoplay_script, ap_out, &ap_err)) {
      std::fprintf(stderr, "carview: --autoplay: %s\n", ap_err.c_str());
      exit_code = 2;
      return false;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
  }
  car_given = !car.empty();
  // The play test's defaults for a window: the bake in out/bake (or
  // out/bake1), the R34, Colorado from SHIFT_GAME_ROOT or the sibling
  // horizon1-recomp checkout, at the free-roam spawn, 1600 x 900.
  // Portable mode keeps the save in the portable folder; SDL's pref path is
  // not asked for (it would create the folder under %APPDATA%).
  const std::filesystem::path portable_dir = carview::portable_user_dir();
  if (!headless && !no_save && save_path.empty()) {
    std::filesystem::path pref_drive;
    if (portable_dir.empty()) {
      if (char* pref = SDL_GetPrefPath("shift-engine", "drive")) {
        pref_drive = std::filesystem::path(pref);
        SDL_free(pref);
      }
    }
    save_path = carview::resolve_default_save_path(portable_dir, pref_drive);
  }
  if (no_save) save_path.clear();
  slice12_modded_save = false;
  for (const auto& a : args) {
    if (a == "--trainer-cheats" || a == "--festival-pacing" || a == "--hidden-cars") slice12_modded_save = true;
  }
  // Mods & Save Ecosystem (mods_page.h, save_sync_ui.h): resolve mods first
  // to know whether an isolated modded save is active (isolated modded saves
  // never sync), boot profiles / Save Sync / rolling backups, then apply
  // modded-save isolation before reading save.txt.
  
  
  {
    const char* sr_env = std::getenv("SHIFT_STATE_ROOT");
    const std::filesystem::path sr = sr_env ? std::filesystem::path(sr_env) : std::filesystem::path{};
    std::error_code ec;
    const bool has_v4 = !sr.empty() && std::filesystem::is_regular_file(sr / "title-update-v4" / "media.zip", ec);
    const auto build = has_v4 ? cars::modmgr::GameBuild::kV4 : cars::modmgr::GameBuild::kBase;
    if (!mods_off) {
      if (mods_root.empty() && !headless) mods_root = carview::default_mods_root();
      mods_session.boot(mods_root, {}, build, sr);
    }
    const bool isolated_mod = slice12_modded_save || (!mods_off && !mods_session.resolution().save_profile.empty());
    if (!save_path.empty()) {
      carview::SaveSession::BootOptions sb;
      sb.user_dir = carview::default_user_dir();
      if (sb.user_dir.empty()) sb.user_dir = save_path.parent_path().parent_path();
      sb.default_save_dir = save_path.parent_path();
      sb.sync_folder = sync_folder;
      sb.device_name = device_name;
      sb.profile_override = profile_name;
      sb.v4_available = has_v4;
      sb.isolated_mod_save = isolated_mod;
      sb.achievements_path = carview::default_achievements_path();
      sb.replays_dir = carview::default_replays_dir();
      const auto prof_dir = save_session.boot(sb);
      save_path = prof_dir / "save.txt";
      clean_user_dir = prof_dir;
      for (const auto& line : save_session.log) std::printf("save: %s\n", line.c_str());
      save_session.log.clear();
    }
    if (!mods_off) {
      const auto dir = mods_session.boot(mods_root, save_path.empty() ? std::filesystem::path{} : save_path.parent_path(),
                                         build, sr);
      if (!save_path.empty()) save_path = dir / "save.txt";
      for (const auto& line : mods_session.log) std::printf("mod: %s\n", line.c_str());
      mods_session.log.clear();
    }
  }
  if (clean_user_dir.empty() && !save_path.empty()) clean_user_dir = save_path.parent_path();
  if (!save_path.empty() && slice12_modded_save && !mods_session.isolation().isolated) {
    save_path = drive::resolve_isolated_save_path(save_path, true);
  }
  // The save: its car when none was given (and it is baked), and where it
  // stood when no spawn was given and the car is the same.
  
  have_saved = false;
  if (!save_path.empty()) {
    std::string e;
    have_saved = drive::read_save(save_path, saved, &e);
    save_read_error = !have_saved && !e.empty();
    if (save_read_error) std::fprintf(stderr, "carview: note: save ignored: %s\n", e.c_str());
  }
  auto resolve_colorado_dir = [](const std::filesystem::path& root) -> std::filesystem::path {
    if (root.empty()) return {};
    std::error_code ec;
    const auto media = std::filesystem::is_directory(root / "media", ec) ? (root / "media") : root;
    for (const char* name : {"Colorado", "colorado"}) {
      const auto d = media / "tracks" / name;
      if (std::filesystem::is_directory(d, ec)) return d;
    }
    return {};
  };
  // The play folder: play.txt ("game <root>", the extracted disc) with optional
  // bake/, sound/ and world/colorado.index beside it. --play names it; otherwise
  // (when --game is not explicitly given) looks in SHIFT_PLAY_DIR, the
  // executable's folder, then the user-data folder (SDL's pref path,
  // shift-engine/play).
  if (game_root_given.empty() && (!headless || bake_dir.empty()) && play_dir.empty()) {
    std::vector<std::filesystem::path> where;
    if (const char* p = std::getenv("SHIFT_PLAY_DIR")) where.emplace_back(p);
    if (const char* b = SDL_GetBasePath()) where.emplace_back(b);
#ifdef SDL_PLATFORM_ANDROID
    // The app's external files folder (Android/data/<app>/files/play).
    if (const char* x = SDL_GetAndroidExternalStoragePath()) where.emplace_back(std::filesystem::path(x) / "play");
#endif
    if (!portable_dir.empty()) {
      where.emplace_back(portable_dir / "play");
    } else if (char* pref = SDL_GetPrefPath("shift-engine", "play")) {
      where.emplace_back(pref);
      SDL_free(pref);
    }
    std::error_code ec;
    for (const auto& d : where)
      if (!d.empty() && std::filesystem::is_regular_file(d / "play.txt", ec)) {
        play_dir = d;
        break;
      }
  }
  std::filesystem::path play_state;
  if (!play_dir.empty()) {
    std::ifstream in(play_dir / "play.txt");
    if (!in) {
      std::fprintf(stderr, "carview: no play.txt in %s (run tools/setup.py)\n", play_dir.string().c_str());
      exit_code = 2; return false;
    }
    std::filesystem::path game;
    for (std::string line; std::getline(in, line);) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("game ", 0) == 0) game = std::filesystem::path(line.substr(5));
      else if (line.rfind("state ", 0) == 0) play_state = std::filesystem::path(line.substr(6));
    }
    if (!game.empty() && game.is_relative()) game = play_dir / game;
    if (!play_state.empty() && play_state.is_relative()) play_state = play_dir / play_state;
    if (game_root_given.empty() && !game.empty()) game_root_given = game;
    std::error_code ec;
    if (bake_dir.empty() && std::filesystem::is_directory(play_dir / "bake" / "cars", ec)) bake_dir = play_dir / "bake";
    if (!sound_given) {
      if (std::filesystem::is_directory(play_dir / "sound", ec)) {
        sound_dir = play_dir / "sound";
      } else {
        // Zero-bake play folder: route runtime sound cache to state_root/sound-cache
        // or play_dir/sound-cache instead of relative CWD "out/sound".
        sound_dir = !play_state.empty() ? (play_state / "sound-cache") : (play_dir / "sound-cache");
      }
    }
    if (cache_path.empty()) cache_path = play_dir / "world" / "colorado.index";
    if (world_dir.empty() && !game.empty()) world_dir = resolve_colorado_dir(game);
    std::printf("play: %s\n", play_dir.string().c_str());
  }
  if (!game_root_given.empty()) {
    if (world_dir.empty()) world_dir = resolve_colorado_dir(game_root_given);
    if (cache_path.empty() && !world_dir.empty()) {
      if (!play_dir.empty()) {
        cache_path = play_dir / "world" / "colorado.index";
      } else if (const auto udir = carview::default_user_dir(); !udir.empty()) {
        cache_path = udir / "cache" / "colorado.index";
      } else {
        cache_path = std::filesystem::path("out") / "world" / "colorado.index";
      }
    }
  }
  if (!cache_path.empty() && cache_path.has_parent_path()) {
    std::error_code cec;
    std::filesystem::create_directories(cache_path.parent_path(), cec);
  }
  if (!headless) {
    std::error_code ec;
    if (bake_dir.empty() && game_root_given.empty())
      for (const char* d : {"out/bake", "out/bake1"})
        if (std::filesystem::is_directory(std::filesystem::path(d) / "cars", ec)) {
          bake_dir = d;
          break;
        }
    if (world_dir.empty()) {
      std::vector<std::filesystem::path> roots;
      if (const char* g = std::getenv("SHIFT_GAME_ROOT")) roots.emplace_back(g);
      roots.emplace_back("../horizon1-recomp/.local/game/base");
      for (const auto& r : roots) {
        const auto d = resolve_colorado_dir(r);
        if (!d.empty()) {
          world_dir = d;
          if (game_root_given.empty()) game_root_given = r;
          break;
        }
      }
    }
    if (!size_given) width = 1600, height = 900;
  }
  state_root = [&] {
    const char* s = std::getenv("SHIFT_STATE_ROOT");
    if (s && *s) return std::filesystem::path(s);
    return play_state;
  }();
  if (!sound_given && sound_dir == "out/sound" && !state_root.empty()) {
    std::error_code ec;
    if (!std::filesystem::is_directory(sound_dir, ec)) sound_dir = state_root / "sound-cache";
  }
  if (const auto groot = game_root_of(); !groot.empty()) {
    Showroom::set_disc_root(groot, state_root);
    drive::set_disc_physics_root(groot, state_root);
  }
  // Mods (cars/mod_manager.h): the window's from the user folder's mods/, a
  // headless run's only from --mods; --no-mods turns them off.
  if (!mods_off) {
    const std::filesystem::path mods_game_root =
        !game_root_given.empty() ? game_root_given
                                 : (!world_dir.empty() ? world_dir.parent_path().parent_path().parent_path()
                                                       : std::filesystem::path{});
    mods_session.activate(bake_dir, sound_dir, mods_game_root);
    mods_session.load_plugins(mods_native);
    carview::set_world_texture_session(&mods_session);
    for (const auto& line : mods_session.log) std::printf("mod: %s\n", line.c_str());
    mods_session.log.clear();
  }
  // Colorado's own features (collectibles, the map, traffic) stay off in
  // another world (ColoradoDirt: SPEC-V4-CONTENT, no traffic on a stage).
  // The parts database (the merged one: SHIFT_STATE_ROOT's packages when
  // set), opened in the background; opened early so load_ui_fonts and garage
  // setup overlap with the SQLite merge.
  
  if (!world_dir.empty()) {
    const auto game_root = game_root_of();
    const std::string sr = state_root.string();
    parts_db = std::async(std::launch::async, [game_root, sr]() -> std::shared_ptr<cars::PartsDb> {
                 cars::Install install;
                 auto db = std::make_shared<cars::PartsDb>();
                 std::string why;
                 if (!cars::discover_install(game_root, sr, "", install, &why) || !db->open(install, &why)) {
                   std::fprintf(stderr, "carview: note: no parts database: %s\n", why.c_str());
                   return nullptr;
                 }
                 return db;
               }).share();
  }
  // The career page's PNGs and the HUD: the game's fonts.
  const auto db_cat_t0 = std::chrono::steady_clock::now();
  if (career_png.empty() && map_png.empty()) {
#ifdef __ANDROID__
    carview::ui::set_atlas_size(low_preset ? 2048u : Renderer::kUiAtlas);
#else
    carview::ui::set_atlas_size(Renderer::kUiAtlas);
#endif
    carview::ui::set_gpu_atlas_mode(true);
  }
  if (!world_dir.empty() || !game_root_given.empty()) load_ui_fonts(game_root_of(), font_cache, state_root);
  // The screen's car names (FORMAT-STRINGTABLES), loaded on first use.
  
  
  carview::set_car_names([this](const std::string& media) {
    if (!*car_names_loaded) {
      *car_names_loaded = true;
      const cars::PartsDb* db = parts_db.valid() ? parts_db.get().get() : nullptr;
      std::string e;
      car_names->load(game_root_of(), state_root, &e, language, db);
    }
    return car_names->short_name(media);
  });
  carview::set_game_text([this](const std::string& value) {
    if (!*car_names_loaded) carview::car_name("");  // loads the tables
    return car_names->text(value);
  });
  // The screens that look text up through ui::lookup_text (the campaign
  // panels, and the paint and replay screens' second try) read the same tables.
  carview::ui::set_text_lookup([](const std::string& ref) { return carview::game_text(ref); });
  // The career garage (SPEC-GARAGE-UPGRADES): garage.txt beside the save,
  // made by the profile import; the cars the bake has, by Data_Car id.
  
  have_garage = false;
  
  if (!save_path.empty()) garage_path = save_path.parent_path() / "garage.txt";
  baked_ids = bake_dir.empty() ? std::map<int64_t, std::string>{} : baked_car_ids(bake_dir);
  std::vector<std::string> disc_cars;
  if (baked_ids.empty()) {
    if (const auto groot = game_root_of(); !groot.empty()) {
      std::string disc_err;
      if (Showroom::discover_disc_cars(groot, state_root, baked_ids, disc_cars, &disc_err)) {
        std::printf("disc: discovered %zu cars (%zu Data_Car rows) directly from %s\n", disc_cars.size(),
                    baked_ids.size(), groot.string().c_str());
      } else if (!disc_err.empty()) {
        std::fprintf(stderr, "carview: note: disc car discovery: %s\n", disc_err.c_str());
      }
    }
  }
  auto car_available = [&](const std::string& media) -> bool {
    if (media.empty()) return false;
    const std::string key = lower_ascii(media);
    if (!bake_dir.empty()) {
      std::error_code e2;
      if (std::filesystem::exists(bake_dir / "cars" / (key + ".shiftbake"), e2)) return true;
    }
    if (!disc_cars.empty() && std::binary_search(disc_cars.begin(), disc_cars.end(), key)) return true;
    for (const auto& [cid, m] : baked_ids)
      if (lower_ascii(m) == key) return true;
    return false;
  };
  if (car.empty() && have_saved && car_available(saved.car)) car = saved.car;
  // The profile import (FORMAT-PROFILE), with the cars the bake has.
  imported = false;
  if (!import_dir.empty() && have_saved && saved.owned_garage && !import_again) {
    // A second import would replace what was bought and driven since.
    std::fprintf(stderr,
                 "carview: the save already holds an imported career (%s); drop --import-profile, or use "
                 "--import-profile-again to replace it\n",
                 save_path.string().c_str());
    exit_code = 2; return false;
  }
  if (!import_dir.empty()) {
    profile::Imported prof;
    std::string e;
    if (!profile::import_profile(import_dir, prof, &e)) {
      std::fprintf(stderr, "carview: --import-profile: %s\n", e.c_str());
      exit_code = 2; return false;
    }
    std::error_code ec;
    const auto& baked = baked_ids;
    std::vector<std::string> notes;
    profile::to_drive_save(prof, [&](int64_t id) { auto it = baked.find(id); return it == baked.end() ? std::string() : it->second; },
                           saved, &notes);
    have_saved = imported = true;
    for (const auto& n : notes) std::printf("import: %s\n", n.c_str());
    std::printf("import: %s: %lld credits, %d wristband points, %zu signs, %zu garage cars, car %s\n",
                import_dir.string().c_str(), (long long)saved.credits, saved.wristband_points, saved.signs_found.size(),
                saved.garage.size(), saved.car.empty() ? "(none baked)" : saved.car.c_str());
    if (!car_given && !saved.car.empty()) car = saved.car;
    if (!save_path.empty() && std::filesystem::exists(save_path, ec)) {
      auto keep = save_path;
      keep += ".pre-import";
      if (!std::filesystem::exists(keep, ec)) std::filesystem::copy_file(save_path, keep, ec);
    }
    career_garage = profile::to_garage(prof);
    have_garage = true;
    if (!garage_path.empty() && std::filesystem::exists(garage_path, ec)) {
      auto keep = garage_path;
      keep += ".pre-import";
      if (!std::filesystem::exists(keep, ec)) std::filesystem::copy_file(garage_path, keep, ec);
    }
    write_career_garage();
  }
  if (!have_garage && have_saved && saved.owned_garage && !garage_path.empty()) {
    std::string e;
    have_garage = garage::read_garage(garage_path, career_garage, &e);
    if (!have_garage && !e.empty()) std::fprintf(stderr, "carview: note: garage ignored: %s\n", e.c_str());
  }
  // The selected garage car is the car (unless --car names one).
  if (have_garage && !car_given)
    if (const garage::Car* gc = career_garage.selected_car())
      if (auto it = baked_ids.find(gc->car_id); it != baked_ids.end()) car = it->second;
  // Default car cleanly when no --car or save/garage car exists.
  if (car.empty()) {
    if (car_available("VIP_Viper_13")) car = "VIP_Viper_13";
    else if (car_available("NIS_SkylineGTRVSpecII_02")) car = "NIS_SkylineGTRVSpecII_02";
    else if (!disc_cars.empty()) car = disc_cars.front();
    else if (!baked_ids.empty()) car = baked_ids.begin()->second;
  }
  if (!headless) {
    std::printf("drive: bake %s, car %s, world %s\n",
                bake_dir.empty() ? "(direct-from-disc)" : bake_dir.string().c_str(), car.c_str(),
                world_dir.empty() ? "(none: flat ground)" : world_dir.string().c_str());
  }
  // Class bands from the disc, and shared merged DB handle for career/race queries.
  
  
  if (!world_dir.empty()) {
    const auto game_root = game_root_of();
    std::string e;
    if (!garage::load_class_bands(game_root, class_bands, &e)) std::fprintf(stderr, "carview: note: %s\n", e.c_str());
    if (parts_db.valid()) shared_db = parts_db.get().get();
  }
  if (!mods_off) {
    const size_t mod_cars = carview::extend_baked_catalog(baked_ids, shared_db, mods_session);
    if (mod_cars > 0) {
      std::printf("mod: extended car catalog with %zu mod car(s)\n", mod_cars);
      if (have_garage && !car_given)
        if (const garage::Car* gc = career_garage.selected_car())
          if (auto it = baked_ids.find(gc->car_id); it != baked_ids.end()) car = it->second;
    }
  }
  // Career progression catalog (events, wristbands, restrictions, world triggers) & Skills/Popularity tables.
  
  
  
  
  if (!world_dir.empty() && colorado_world()) {
    std::string e;
    have_career_catalog = race::load_career_catalog(game_root_of(), state_root, shared_db, career_catalog, &e);
    if (!have_career_catalog && !e.empty()) std::fprintf(stderr, "carview: note: no career catalog: %s\n", e.c_str());
    if (have_career_catalog) campaign.set_career_catalog(&career_catalog);
    e.clear();
    // The campaign's wristband, Star Showdown and Showcase tables. The raw
    // text: game_text strips "{0}" placeholders, and the wristband name
    // goes into Scoreboard's "{0} WRISTBAND".
    race::set_campaign_caption_resolver(
        [](std::string_view ref) { return carview::game_text_raw(std::string(ref)); });
    race::CampaignTablesLoad tl;
    if (race::load_campaign_tables(game_root_of(), state_root, shared_db,
                                   [](std::string_view ref) { return carview::game_text_raw(std::string(ref)); }, &tl,
                                   &e))
      std::printf("campaign: %d wristbands (%s..), %d star showdowns, %d showcases from the disc (%d captions missing)\n",
                  tl.wristbands, race::wristband_milestones().front().display_name.c_str(), tl.bosses, tl.showcases,
                  tl.captions_missing);
    else if (!e.empty())
      std::fprintf(stderr, "carview: note: built-in campaign tables: %s\n", e.c_str());
    e.clear();
    have_skills_data = freeroam::load_skills_data(game_root_of(), skills_data, &e);
    if (!have_skills_data && !e.empty()) std::fprintf(stderr, "carview: note: no skills data: %s\n", e.c_str());
    e.clear();
    if (!freeroam::load_free_roam_challenges(game_root_of(), freeroam_challenges_data, &e) && !e.empty())
      std::fprintf(stderr, "carview: note: no free-roam challenges: %s\n", e.c_str());
  }
  bench_startup.db_catalog_load_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - db_cat_t0).count();
  // Title screen shown on windowed launch unless bypassed (--no-title, --start-in-single-player, --event, --import-profile).
  show_title =
      cli_title_screen || (!headless && !no_title && !start_in_single_player && race_event.empty() && import_dir.empty());
  
  
  
  
  
  campaign.configure(game_root_of(), language, skip_intro_movies);
  // An original profile stores no campaign mirror: rebuild it from the
  // imported career (bosses, showcases by popularity rank, finale, the
  // one-shot lines already passed) so nothing replays or relocks (PROG-15).
  // CampaignDirector::load_from_save does the same on every load path.
  const int saved_pop_rank =
      have_saved && have_skills_data ? skills_data.rank_for_points(saved.popularity_points) : 0;
  if (have_saved && race::imported_campaign_needs_seed(saved)) race::seed_imported_campaign(saved, saved_pop_rank);
  if (imported)
    std::printf("import: popularity %.0f (rank %d), wristband %d collected, %zu bosses, %zu showcases, finale %s\n",
                saved.popularity_points, saved_pop_rank, saved.collected_wristband, saved.unlocked_bosses.size(),
                saved.unlocked_showcases.size(), saved.finale_complete ? "won" : "open");
  if (have_saved) campaign.load_from_save(saved, saved_pop_rank);
  title.reset();
  if (cli_title_menu) title.show_menu();
  title.profile_error = save_read_error;
  // Default Subtitles: DISABLE when UI language matches audio language, ENABLE otherwise.
  const bool default_subs = false;
  if (have_saved) {
    if (!display_brightness_explicit) display_brightness = carview::TitleMenu::snap_brightness(saved.brightness);
    if (!subtitles_explicit) subtitles_enabled = saved.has_subtitles ? saved.subtitles : default_subs;
  } else {
    display_brightness = carview::TitleMenu::snap_brightness(display_brightness);
    if (!subtitles_explicit) subtitles_enabled = default_subs;
  }
  title.brightness = display_brightness;
  title.default_subtitles = default_subs;
  title.subtitles = subtitles_enabled;
  fmv_player.set_subtitles_enabled(subtitles_enabled);
  // Boot gate (Main/Flags bit 0x40000; Q276, Q363, first30l vid1/vid2): show VIDEO
  // only when the profile has not yet saved past VIDEO.
  title.first_profile = (!have_saved || !saved.video_seen) && !save_read_error;
  if (!race_event.empty()) {
    if (world_dir.empty()) {
      std::fprintf(stderr, "carview: --event needs the world (--world)\n");
      exit_code = 2; return false;
    }
    std::string why;
    if (!start_race(race_event, why)) {
      std::fprintf(stderr, "carview: --event %s: %s\n", race_event.c_str(), why.c_str());
      exit_code = 2; return false;
    }
  }
  // The career (SPEC-EVENT-REWARDS) from the save.
  if (have_saved) {
    career.credits = saved.credits, career.total_winnings = saved.total_winnings;
    career.wristband_points = saved.wristband_points;
    career.num_races = saved.num_races, career.num_victories = saved.num_victories, career.num_podiums = saved.num_podiums;
    career.time_in_festival = saved.time_festival, career.time_in_street = saved.time_street;
    career.time_in_exhibition = saved.time_exhibition, career.top_speed = saved.top_speed;
    for (const auto& [id, place] : saved.race_results) career.race_places[id] = place;
    for (const auto& [id, place] : saved.event_results) career.event_places[id] = place;
    career.prize_cars = saved.prize_cars;
    if (saved.difficulty >= 0 && saved.difficulty <= 4) race_options.difficulty = race::Difficulty(saved.difficulty);
    if (saved.ai_mode >= 0 && saved.ai_mode <= 2) race_options.ai_mode = race::AiMode(saved.ai_mode);
    // Restore any in-progress Barn Find restorations saved before quitting (skipping cars already in garage).
    pending_barn_restorations.clear();
    barn_restorations_reconciled = false;
    for (const auto& br : saved.pending_barn_restorations) {
      if (br.car_id <= 0) continue;
      bool already_owned = false;
      for (const auto& gc : career_garage.cars) {
        if (gc.car_id == br.car_id) {
          already_owned = true;
          break;
        }
      }
      if (already_owned) continue;
      char bbuf[32];
      std::snprintf(bbuf, sizeof bbuf, "barnfind_%02d", std::max(1, br.barn_id));
      std::string cname;
      if (auto it = baked_ids.find(br.car_id); it != baked_ids.end()) cname = carview::car_name(it->second);
      if (cname.empty() && have_career_catalog) {
        if (const auto* sp = career_catalog.find_car(br.car_id)) cname = carview::car_name(sp->media_name);
      }
      if (cname.empty()) cname = "Barn Find #" + std::to_string(br.barn_id);
      const double rem = std::max(0.0, double(br.remaining_sec));
      const bool ready = (rem <= 0.0);
      pending_barn_restorations.push_back({bbuf, cname, int64_t(br.car_id), rem, 0.0, ready, ready});
    }
  }
  // Sync dynamic_ai_skill to the player's chosen difficulty band so Stock mode never reverts Hard/Pro/Easy back to Med.
  if (have_saved) {
    dynamic_ai_skill = saved.ai_skill;
    const auto want_tier =
        race_options.difficulty == race::Difficulty::kHardPlus ? race::Difficulty::kPro : race_options.difficulty;
    if (race::difficulty_from_dynamic_skill(dynamic_ai_skill) != want_tier) {
      dynamic_ai_skill = dynamic_skill_for_difficulty(race_options.difficulty);
    }
  } else {
    dynamic_ai_skill = dynamic_skill_for_difficulty(race_options.difficulty);
  }
  if (wristbands.levels.empty() && !world_dir.empty()) {
    std::string e;
    race::load_wristband_tables(game_root_of(), state_root, shared_db, wristbands, &e);
  }
  
  
  if (!world_dir.empty()) {
    std::string pe;
    have_prog_tables = race::load_progression_tables(game_root_of(), state_root, shared_db, prog_tables, &pe);
    if (have_prog_tables)
      std::printf("progression: %zu driver levels, %zu affinity levels loaded\n", prog_tables.driver_levels.size(),
                  prog_tables.affinity_levels.size());
  }
  if (have_saved) {
    career.driver_xp = saved.driver_xp;
    career.driver_level =
        race::resume_driver_level(have_prog_tables ? &prog_tables : nullptr, saved.level, career.driver_xp);
    for (const auto& [mid, xp] : saved.make_affinity) career.make_affinity_xp[mid] = xp;
  }
  h2h_races = have_saved ? saved.h2h_races : 0;
  h2h_wins = have_saved ? saved.h2h_wins : 0;
  
  
  const bool is_colorado_or_dirt =
      colorado_world() || lower_ascii(world_dir.filename().string()) == "coloradodirt";
  if (!world_dir.empty() && is_colorado_or_dirt) {
    std::string se;
    // ColoradoDirt (<package>/Media/Tracks/ColoradoDirt) reads its own set
    // from the package's Media/gamemodes/ColoradoDirt.
    const std::filesystem::path speed_dir =
        colorado_world() ? std::filesystem::path()
                         : world_dir.parent_path().parent_path() / "gamemodes" / "ColoradoDirt";
    if (freeroam::load_speed_activities(game_root_of(), speed_data, &se, speed_dir)) {
      speed_tracker = std::make_unique<freeroam::SpeedActivityTracker>(speed_data);
      if (have_saved) speed_tracker->load_records(saved.speed_traps, saved.speed_zones);
      std::printf("speed activities: %zu traps, %zu zones loaded\n", speed_data.traps.size(), speed_data.zones.size());
    }
  }
  collected_wristband = 0;
  notified_wristband = -2;  // re-seeded from this career on the next step
  if (have_saved && saved.collected_wristband >= 0) {
    collected_wristband = saved.collected_wristband;
  } else if (have_saved && !saved.intro_completed && saved.collected_wristband < 0) {
    collected_wristband = -1;
  } else if (have_career_catalog) {
    collected_wristband = std::max(0, career_catalog.earned_wristband(career.wristband_points));
  } else {
    collected_wristband = std::max(0, wristbands.level_index(career.wristband_points));
  }
  
  max_skills_car = have_saved ? saved.max_skills_car : 0;
  
  
  if (have_skills_data) {
    skill_tracker = std::make_unique<freeroam::SkillTracker>(
        skills_data, have_saved ? saved.popularity_points : 0.0, have_saved ? saved.fame_rewards_claimed : 0,
        have_saved ? saved.max_skills_score : 0, max_skills_car);
    if (have_saved) {
      for (const auto& sp : saved.sponsors) skill_tracker->set_sponsor_progress(sp.id, sp.count, sp.claimed);
    }
  }
  if (!freeroam_challenges_data.challenges.empty()) {
    freeroam_challenges = std::make_unique<freeroam::FreeRoamChallengeTracker>(freeroam_challenges_data);
    std::vector<freeroam::ChallengeRecord> recs;
    if (have_saved)
      for (const auto& fc : saved.freeroam_challenges) recs.push_back({fc.id, fc.progress, fc.completed});
    freeroam_challenges->load(recs);
  }
  // --career-png: every window of the career page as the game's save left
  // it (prefix_N.png), then exit: the M6 gate's view of the import.
  if (!career_png.empty()) {
    std::vector<cars::PackInfo> packs;
    const carview::CareerSummary cs = career_summary(packs);
    carview::CareerPage page;
    page.open(cs, std::move(packs));
    for (int n = 0; n < 8; ++n) {
      HudState st;
      int sel = 0;
      page.fill(st.menu_title, st.menu, sel);
      st.menu_selected = sel;
      const std::string f = career_png.string() + "_" + std::to_string(n) + ".png";
      const carview::CareerBoard board = career_board(page);
      if (!write_menu_png(st, f, &board)) {
        exit_code = 1;
        return false;
      }
      std::printf("career: %s\n", f.c_str());
      if (n == 0)
        std::printf("career:   %s  %d points (next %d)  %lld CR  winnings %lld CR  races %d wins %d podiums %d  "
                    "garage %d  signs %d  barns %d/%d  club %d cars %d/%d  rally %s level %d %d xp %d/%d\n",
                    board.band_name.empty() ? "no wristband" : board.band_name.c_str(), board.points, board.band_hi,
                    static_cast<long long>(board.credits), static_cast<long long>(board.winnings), board.races,
                    board.wins, board.podiums, board.garage, board.signs, board.barns, board.barns_total,
                    board.club_cars, board.club_done, board.club_total, board.rally ? "yes" : "no", board.rally_level,
                    board.rally_xp, board.rally_wins, board.rallies);
      for (const auto& l : st.menu) std::printf("career:   %s\n", l.c_str());
      const size_t before = st.menu.empty() ? 0 : st.menu.size();
      page.move(7);
      HudState probe;
      int sel2 = 0;
      page.fill(probe.menu_title, probe.menu, sel2);
      if (probe.menu == st.menu || before == 0) break;
    }
    exit_code = 0; return false;
  }
  title_saved_resume_pending = false;
  
  if (have_saved && !imported && !have_spawn && (!car_given || lower_ascii(saved.car) == lower_ascii(car))) {
    saved_resume_spawn[0] = saved.position[0];
    saved_resume_spawn[1] = saved.position[1];
    saved_resume_spawn[2] = saved.position[2];
    saved_resume_spawn[3] = saved.heading;
    const bool has_press_start_video =
        !FmvPlayer::resolve_movie(game_root_of(), FmvMovieId::kPressStart, state_root).empty();
    if (show_title && !cli_spawn_given && !world_dir.empty() && colorado_world() && !has_press_start_video) {
      // Only showcase the scenic Colorado highway when PressStart.wmv is absent;
      // when PressStart.wmv covers the title screen, load directly at the saved spot so
      // CONTINUE CAREER enters the world instantaneously with zero re-gather.
      title_saved_resume_pending = true;
    } else {
      spawn[0] = saved.position[0], spawn[1] = saved.position[1], spawn[2] = saved.position[2];
      spawn[3] = saved.heading;
      have_spawn = true;
    }
    std::printf("save: %s, %s at %.1f %.1f %.1f\n", save_path.string().c_str(), saved.car.c_str(),
                saved.position[0], saved.position[1], saved.position[2]);
  }
  if ((bake_dir.empty() && game_root_of().empty()) || car.empty()) {
    std::fprintf(stderr, "carview: no game/bake and car (pass --game <dir> or --play <dir>)\n");
    drive_usage();
    exit_code = 2; return false;
  }
  if (view != "chase" && view != "front" && view != "cockpit") {
    std::fprintf(stderr, "carview --drive: --view is chase, front, or cockpit\n");
    exit_code = 2; return false;
  }
  front = view == "front";
  if (view == "cockpit") camera_mode = int(drive::CameraMode::kCockpit);
  cruise = run == "cruise" && script_path.empty();
  if (cruise && world_dir.empty()) {
    std::fprintf(stderr, "carview --drive: --run cruise needs --world\n");
    exit_code = 2; return false;
  }
  if (headless && out.empty()) out = car + "_drive.png";
  
  if (!world_reference_path.empty()) {
    if (!headless || world_dir.empty()) {
      std::fprintf(stderr, "carview --drive: --world-reference needs --headless and --world\n");
      exit_code = 2; return false;
    }
    std::string e;
    if (!read_world_reference(world_reference_path, world_reference, e)) {
      std::fprintf(stderr, "carview: %s\n", e.c_str());
      exit_code = 1; return false;
    }
    // Gather around the camera (game frame: the file frame with z negated).
    near_x = world_reference.camera_position[0];
    near_z = -world_reference.camera_position[2];
    near_given = true;
    width = world_reference.width;
    height = world_reference.height;
  }
  std::string text;
  if (!script_path.empty()) {
    std::ifstream in(script_path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  } else {
    for (const auto& r : kDriveRuns)
      if (run == r[0]) text = r[1];
    if (text.empty()) {
      std::fprintf(stderr, "carview --drive: no run named %s\n", run.c_str());
      exit_code = 2; return false;
    }
  }
  
  if (!drive::parse_script(text, script, &error)) {
    std::fprintf(stderr, "carview --drive: %s\n", error.c_str());
    exit_code = 1; return false;
  }

  
  if (!headless) {
    carview::apply_sdl_gamepad_hints();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
      std::fprintf(stderr, "carview: SDL_Init: %s\n", SDL_GetError());
      exit_code = 1; return false;
    }
    carview::load_gamepad_mappings();
    // Audio apart: without it the drive goes on silent.
    if (sound && !SDL_InitSubSystem(SDL_INIT_AUDIO))
      std::fprintf(stderr, "carview: no audio (%s); driving without sound\n", SDL_GetError());
    window = SDL_CreateWindow(("carview drive - " + car).c_str(), int(width), int(height),
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
      std::fprintf(stderr, "carview: SDL_CreateWindow: %s\n", SDL_GetError());
      SDL_Quit();
      exit_code = 1; return false;
    }
    SDL_ShowWindow(window);
    SDL_PumpEvents();
  }
  status = 0;
  // The car picker (ours): the baked cars by key; picking one restarts the
  // drive with it where the last car stood (window and SDL kept). With a
  // save from the player's profile, only the owned cars the bake has (the
  // garage, barn finds included).
  
  
  
  
  {
    std::error_code ec;
    if (!bake_dir.empty()) {
      for (const auto& e : std::filesystem::directory_iterator(bake_dir / "cars", ec))
        if (e.path().extension() == ".shiftbake") baked_cars.push_back(e.path().stem().string());
    }
    if (baked_cars.empty() && !disc_cars.empty()) {
      baked_cars = disc_cars;
    }
    std::sort(baked_cars.begin(), baked_cars.end());
    for (const auto& c : baked_cars) all_baked.insert(lower_ascii(c));
    if (have_garage) {
      const size_t n = garage_picker();
      std::printf("garage: %zu cars, %zu baked\n", career_garage.cars.size(), n);
    } else if (have_saved && saved.owned_garage) {
      const auto& ids = baked_ids;
      std::vector<std::string> owned;
      for (int id : saved.garage)
        if (auto it = ids.find(id); it != ids.end()) owned.push_back(lower_ascii(it->second));
      std::sort(owned.begin(), owned.end());
      owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
      std::vector<std::string> keep;
      for (const auto& c : baked_cars)
        if (std::binary_search(owned.begin(), owned.end(), lower_ascii(c))) keep.push_back(c);
      std::printf("garage: %zu owned cars, %zu baked\n", saved.garage.size(), keep.size());
      if (!keep.empty()) baked_cars = std::move(keep);
    }
  }
  
  // The pause menu's event pick (ours): every race carview runs, by
  // HorizonEventID; picking one restarts the drive on its grid. After a
  // finish, Continue leaves the race at the route's post-race spot and
  // Restart runs it again (SPEC-EVENT-REWARDS, Results screen).
  
  
  if (!world_dir.empty() && colorado_world()) {
    std::vector<race::EventSummary> all;
    std::string e;
    if (race::list_events(game_root_of(), all, &e, state_root, shared_db))
      for (const auto& ev : all)
        if (ev.ribbon >= 0 && ev.career_type != 9 && ev.environment_id != 111)
          event_ids.push_back(ev.horizon_id), event_names[ev.horizon_id] = ev.name;
  } else if (!world_dir.empty()) {
    // Another world (ColoradoDirt): its stages, one per route file with a
    // loose AI line (SPEC-V4-CONTENT; the heats are route 24).
    std::string w = world_dir.filename().string();
    for (char& ch : w) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (int r = 0; r < 300; ++r) {
      char route[40], line[40];
      std::snprintf(route, sizeof route, "TrackRoute%03d.xml", r);
      std::snprintf(line, sizeof line, "route_%03d.owt", r);
      std::error_code ec;
      if (std::filesystem::exists(world_dir / "Ribbon_00" / route, ec) &&
          std::filesystem::exists(world_dir.parent_path().parent_path() / "aiopenworld" / w / "Ribbon_00" / line, ec))
        event_ids.push_back("stage:" + std::to_string(r));
    }
  }
  
  for (size_t k = 0; k < event_ids.size(); ++k)
    if (event_ids[k] == race_event) event_pick = k;
  
  
  // The free-roam progress outlives car changes; the map loads once.
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  // The audio host (dedicated render thread, mixer, radio, Your Music)
  // outlives car changes so the radio stations keep playing.
  
  
  if (have_saved) {
    if (audio_cfg.radio_dj && !saved.radio_dj) audio_cfg.radio_dj = false;
    if (std::fabs(audio_cfg.radio_volume - 1.0f) < 1e-4f && std::fabs(saved.radio_volume - 1.0) > 1e-4)
      audio_cfg.radio_volume = float(saved.radio_volume);
    if (audio_cfg.satnav_on && !saved.satnav_enabled) audio_cfg.satnav_on = false;
    if (saved.treasure_map) treasure_map_setting = true;
    outpost_progress.discovered_outposts.insert(saved.discovered_outposts.begin(), saved.discovered_outposts.end());
    outpost_progress.completed_pr_stunts.insert(saved.completed_pr_stunts.begin(), saved.completed_pr_stunts.end());
  }
  user_music_dir =
      !state_root.empty() ? (state_root / "music")
                          : (!settings_path().empty() ? settings_path().parent_path() / "music" : std::filesystem::path{});
  audio_host.apply_config(audio_cfg, user_music_dir);
  if (have_saved && !hour_explicit && saved.tod_sec >= 0.0 && saved.tod_sec < 86400.0) {
    tod_clock.tod_sec = saved.tod_sec;
  } else {
    tod_clock.set_hour(hour);
  }
  // An --event started above set its start time; the saved or --hour time
  // must not overwrite it.
  snap_to_event_tod(race_event);
  tod_clock.speed_scale = day_length_scale;
  colorado_world_dir = world_dir;
  start_world_assets_async(true);

  return true;
}

void DriveApp::start_world_assets_async(bool init_anim) {
  wait_all_world_assets();
  const auto groot = game_root_of();
  const auto wdir = world_dir;
  const auto cwdir = colorado_world_dir;
  if (!groot.empty()) {
    const auto media = groot / "media";
    fut_vfx_ = std::async(std::launch::async, [this, media, wdir]() {
      std::string vfx_err;
      return vfx_lib.load(media, wdir, &vfx_err);
    });
    fut_smash_ = std::async(std::launch::async, [this, media, wdir]() {
      std::string sm_err;
      return smashables.load(media, wdir, &sm_err);
    });
  }
  tod_system = world::TodSystem{};
  if (!wdir.empty()) {
    fut_tod_ = std::async(std::launch::async, [this, wdir, cwdir]() {
      std::string tod_err;
      bool tod_ok = tod_system.load(wdir, &tod_err);
      if (!tod_ok && !cwdir.empty() && wdir != cwdir) {
        tod_ok = tod_system.load(cwdir, &tod_err);
      }
      return tod_ok;
    });
  }
  if (!groot.empty()) {
    const auto media = groot / "media";
    fut_anim_ = std::async(std::launch::async, [this, media, wdir, init_anim]() {
      // Wait for ToD post_config? Only read post_config after wait_tod_ready or load with default if concurrent.
      // world_anim reads pgeo/xds/crowds; post_config only supplies tint fallback, so use safe default here
      // and update world_anim if needed.
      std::string anim_err;
      const decltype(tod_system.post_config) default_post{};
      if (init_anim) {
        return world_anim.init(media, default_post, &anim_err, wdir);
      }
      world_anim.set_world(media, wdir, default_post);
      return true;
    });
    anim_shared_bound_ = false;
  }
}

void DriveApp::wait_tod_ready() {
  if (fut_tod_.valid()) {
    if (fut_tod_.get()) {
      std::printf("tod: TimeOfDayA (%zu curves), zones %zu, LUTs %zu, sky %s (%zu stars)\n",
                  tod_system.preset_a.curves.size(), tod_system.zones.zones.size(), tod_system.zones.luts.size(),
                  tod_system.loaded_sky ? "realtimesky.zip" : "procedural", tod_system.sky_assets.stars.size());
    }
  }
}

void DriveApp::wait_vfx_ready() {
  if (fut_vfx_.valid()) {
    if (fut_vfx_.get()) {
      std::printf("vfx: %zu effects, %zu xds textures, %zu world emitters, %zu smashable effects\n",
                  vfx_lib.effects().size(), vfx_lib.xds_count(), vfx_lib.world_emitters().size(),
                  vfx_lib.smashable_effects().size());
    }
  }
}

void DriveApp::wait_smash_ready() {
  if (fut_smash_.valid()) {
    if (fut_smash_.get()) {
      const auto c = smashables.definitions().counts();
      std::printf("smashables: %zu defs (%zu children), %zu CollObjects types, %zu placed props\n",
                  c.top_level, c.total_children, smashables.coll_object_types().size(),
                  smashables.placed_prop_count());
    }
  }
}

void DriveApp::wait_anim_ready() {
  if (fut_anim_.valid()) {
    if (fut_anim_.get() && !anim_shared_bound_) {
      const auto& cat = world_anim.catalog();
      std::printf(
          "world_anim: %zu spectator models (%d angles, %zu sit), %zu anim clips, %zu crowdclasses, "
          "%zu animated pgeo (%zu decoded) / %zu xds\n",
          cat.models.size(), cat.num_sprite_angles, cat.sit_assets.size(), cat.anim_clips.size(),
          cat.crowdclasses.size(), cat.animated_pgeos, cat.pgeo_objects.size(), cat.animated_xds);
      world::AnimatedWorldSystem::set_shared(&world_anim);
      anim_shared_bound_ = true;
    }
  }
}

void DriveApp::wait_all_world_assets() {
  wait_tod_ready();
  wait_vfx_ready();
  wait_smash_ready();
  wait_anim_ready();
}

void DriveApp::reload_world_assets(bool init_anim) {
  start_world_assets_async(init_anim);
  wait_all_world_assets();
}

bool DriveApp::handle_rally_world_switch() {
  if (status == 0 && rally_world_switch != 0) {
    const int dir = rally_world_switch;
    rally_world_switch = 0;
    race.reset();
    race_event.clear();
    bool world_changed = false;
    if (dir > 0 && !rally_catalog.package_dir.empty()) {
      world_dir = rally_catalog.package_dir / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "Tracks" / "ColoradoDirt";
      race_options.stage_world = world_dir;
      race_options.rally_package = rally_catalog.package_dir;
      cache_path.clear();
      have_spawn = false;
      rally_hub_open = true;
      rally_hub_selected = 0;
      rally_notice.clear();
      // ColoradoDirt's own traps and zones (the package's gamemodes), in
      // place of Colorado's (V4-18).
      {
        std::string se;
        freeroam::SpeedActivitiesData dirt;
        if (freeroam::load_speed_activities(game_root_of(), dirt, &se,
                                            freeroam::colorado_dirt_gamemodes(rally_catalog.package_dir))) {
          if (speed_tracker) {
            saved.speed_traps = speed_tracker->save_traps();
            saved.speed_zones = speed_tracker->save_zones();
          }
          speed_data = std::move(dirt);
          speed_tracker = std::make_unique<freeroam::SpeedActivityTracker>(speed_data);
          if (have_saved) speed_tracker->load_records(saved.speed_traps, saved.speed_zones);
          std::printf("speed activities (ColoradoDirt): %zu traps, %zu zones loaded\n", speed_data.traps.size(),
                      speed_data.zones.size());
        } else {
          std::fprintf(stderr, "speed activities (ColoradoDirt): %s\n", se.c_str());
        }
      }
      world_changed = true;
      std::printf("rally: switched world to ColoradoDirt (%s)\n", world_dir.string().c_str());
      // The first visit (game_rally_intro_flow): after the video, the intro
      // race, RALLY_HEATS in its loaner, before any hub.
      if (saved.rally_flow == 1 && !race::is_rally_heats_completed(saved)) {
        if (const std::string id = start_rally_heats(); !id.empty()) {
          std::string why;
          if (start_race(id, why)) {
            race_event = id;
            loading_into_event = true;
            std::printf("rally: the intro race (%s)\n", id.c_str());
          } else {
            std::fprintf(stderr, "rally: intro race %s: %s\n", id.c_str(), why.c_str());
            rally_session.retire();
            rally_hub_open = true;
            if (!pre_loaner_car.empty()) {
              car = pre_loaner_car;
              if (pre_loaner_garage_id > 0) career_garage.selected = pre_loaner_garage_id;
              pre_loaner_car.clear();
              pre_loaner_garage_id = 0;
            }
          }
        }
      } else if (saved.rally_flow == 2) {
        // Left before the first Autoshow: it opens on the way back in.
        rally_hub_open = false;
        rally_first_autoshow_pending = true;
      }
    } else if (dir < 0 && !colorado_world_dir.empty()) {
      world_dir = colorado_world_dir;
      race_options.stage_world = world_dir;
      cache_path.clear();
      if (has_colorado_return) {
        spawn[0] = colorado_return_pos[0];
        spawn[1] = colorado_return_pos[1];
        spawn[2] = colorado_return_pos[2];
        spawn[3] = colorado_return_heading;
        have_spawn = true;
      }
      rally_session.retire();
      rally_hub_open = false;
      rally_service_open = false;
      rally_service_return = false;
      rally_service_resume_after_reload = false;
      rally_results_open = false;
      rally_first_autoshow_pending = false;
      rally_first_autoshow_open = false;
      {
        std::string se;
        freeroam::SpeedActivitiesData base;
        if (freeroam::load_speed_activities(game_root_of(), base, &se)) {
          if (speed_tracker) {
            saved.speed_traps = speed_tracker->save_traps();
            saved.speed_zones = speed_tracker->save_zones();
          }
          speed_data = std::move(base);
          speed_tracker = std::make_unique<freeroam::SpeedActivityTracker>(speed_data);
          if (have_saved) speed_tracker->load_records(saved.speed_traps, saved.speed_zones);
        }
      }
      world_changed = true;
      std::printf("rally: returned to Colorado (%s)\n", world_dir.string().c_str());
    }
    if (world_changed) {
      reload_world_assets(false);
    }
    return true;
  }
  return false;
}

void DriveApp::shutdown() {
  wait_all_world_assets();
  carview::set_car_names({});
  carview::set_game_text({});
  carview::ui::set_text_lookup({});
  carview::set_world_texture_session(nullptr);
  mods_session.shutdown();
  if (window) {
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
}

}  // namespace carview
