// Slice 7 Milestone 1 unit tests:
// - PI.xml virtual-lap parser, Euler-spiral segments, lap simulation, and PI/class calculation
// - Oracle accuracy check against game disc + reference upgraded cars when assets are present
// - Engine / aspiration conversions, Auto Upgrade (up & down), car/part selling (50%)
// - 35-slider CarTune -> CarParams wiring and garage.txt v2 round-trip persistence
#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/from_physics.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "garage/tuning.h"
#include "race/player_pi.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static std::vector<garage::ClassBand> standard_bands() {
  return {{0.005, 99, "F"},   {0.459, 200, "E"},  {0.539, 300, "D"},
          {0.5975, 400, "C"}, {0.6505, 500, "B"}, {0.739, 600, "A"},
          {0.81, 700, "S"},   {0.8665, 800, "R3"}, {0.8991, 900, "R2"},
          {0.999999999999, 999, "R1"}, {1.0, 999, "X"}};
}

static void test_synthetic_pi_and_tuning() {
  using namespace garage;
  const auto bands = standard_bands();

  // 1. Parse synthetic PI.xml & verify time-to-PI mapping.
  const std::string xml = R"(
    <PI>
      <Misc MinPITimeSeconds="138.24" MaxPITimeSeconds="279.9" TrackWidthMeters="9"
            FWDAccelFrictionScaleWhileCornering="0.91" RWDAccelFrictionScaleWhileCornering="0.85"
            AWDAccelFrictionScaleWhileCornering="1.0" FWDAccelFrictionScaleWhileStraight="0.955"
            RWDAccelFrictionScaleWhileStraight="0.925" AWDAccelFrictionScaleWhileStraight="1.0"
            FWDLapTimeScale="1.0" RWDLapTimeScale="1.0" AWDLapTimeScale="1.0"/>
      <Gears NumForwardGears="4">
        <Gear RedlineSpeedMPH="50"/>
        <Gear RedlineSpeedMPH="90"/>
        <Gear RedlineSpeedMPH="140"/>
        <Gear RedlineSpeedMPH="200"/>
      </Gears>
      <Track>
        <Segment StraightLengthMeters="600.0" CornerRadiusMeters="90" CornerAngleDegrees="90"/>
        <Segment StraightLengthMeters="350.0" CornerRadiusMeters="45" CornerAngleDegrees="120"/>
        <Segment StraightLengthMeters="450.0" CornerRadiusMeters="120" CornerAngleDegrees="60"/>
      </Track>
    </PI>)";
  PiConfig cfg;
  std::string err;
  CHECK(parse_pi_xml(xml, cfg, &err));
  CHECK(cfg.segments.size() == 3);
  CHECK(cfg.gear_redline_mph.size() == 4);
  CHECK(std::fabs(lap_time_to_pi(138.24, cfg) - 1.0) < 1e-6);
  CHECK(std::fabs(lap_time_to_pi(279.90, cfg) - 0.0) < 1e-6);
  CHECK(std::fabs(lap_time_to_pi(350.0, cfg) - 0.0) < 1e-6);

  // Higher power & lower mass must produce a strictly faster lap time and higher PI.
  PiConfig synth_cfg = cfg;
  synth_cfg.min_pi_time_s = 20.0;
  synth_cfg.max_pi_time_s = 120.0;
  drive::CarParams base_p;
  base_p.tyre_radius = {0.32, 0.32, 0.32, 0.32};
  base_p.wheel_centres = {{{-0.8, 0.32, 1.3}, {0.8, 0.32, 1.3}, {-0.8, 0.32, -1.3}, {0.8, 0.32, -1.3}}};
  base_p.mass = 1500.0;
  base_p.torque_scale = 300.0;
  drive::CarParams fast_p = base_p;
  fast_p.mass = 1150.0;
  fast_p.torque_scale = 520.0;
  const double t_base = simulate_pi_lap(base_p, nullptr, synth_cfg);
  const double t_fast = simulate_pi_lap(fast_p, nullptr, synth_cfg);
  CHECK(t_fast < t_base - 3.0);
  CHECK(lap_time_to_pi(t_fast, synth_cfg) > lap_time_to_pi(t_base, synth_cfg));

  // 2. Car selling & Part selling (50% resale value).
  Garage g;
  g.last_id = 2;
  g.selected = 1;
  g.cars.push_back({1, 309, 0.52, 2, 0, 1, {{"Brakes", 309000}}});
  g.cars.push_back({2, 445, 0.63, 4, 4000, 1, {{"Brakes", 445003}}});
  g.bought.push_back({2, 4, 445003, 142, 4000});

  int64_t credits = 10000;
  CHECK(sell_car_value(50000, 4000) == 27000);
  // Cannot sell currently selected car.
  CHECK(!sell_car(g, 1, 15000, credits).ok && credits == 10000);
  // Sell part on car 2 -> 50% of 4000 = 2000 CR, reverts slot to stock.
  auto sp = sell_part(g, 2, 4, 445003, 445000, "Brakes", credits);
  CHECK(sp.ok && sp.payout == 2000 && credits == 12000);
  CHECK(g.find(2)->slots["Brakes"] == 445000 && g.find(2)->parts_value == 0 && g.bought.empty());
  // Sell non-selected car 2 -> 50% of 50000 = 25000 CR.
  auto sc = sell_car(g, 2, 50000, credits);
  CHECK(sc.ok && sc.payout == 25000 && credits == 37000 && g.cars.size() == 1);
  // Cannot sell last car.
  g.selected = 999;
  CHECK(!sell_car(g, 1, 15000, credits).ok);

  // 3. CarTune sliders -> CarParams & garage.txt v2 round-trip.
  cars::CarPhysics synth_phys;
  synth_phys.values["List_UpgradeCarBodyWeight.Mass"] = 1400.0;
  synth_phys.values["List_UpgradeDrivetrainTransmission.Level"] = 3.0;
  synth_phys.values["List_UpgradeDrivetrainTransmission.NumGears"] = 7.0;
  synth_phys.values["List_UpgradeDrivetrainTransmission.FinalDriveRatio"] = 3.7;
  for (int i = 1; i <= 6; ++i)
    synth_phys.values["List_UpgradeDrivetrainTransmission.GearRatio" + std::to_string(i)] = 3.5 - 0.45 * i;
  synth_phys.values["List_UpgradeSpringDamper.Level"] = 3.0;
  synth_phys.values["FrontSpringDamper.DefSpringRate"] = 45.0;
  synth_phys.values["FrontSpringDamper.MinSpringRate"] = 25.0;
  synth_phys.values["FrontSpringDamper.MaxSpringRate"] = 90.0;
  synth_phys.values["FrontSpringDamper.DefRideHeight"] = 0.16;
  synth_phys.values["FrontSpringDamper.MinRideHeight"] = 0.11;
  synth_phys.values["FrontSpringDamper.MaxRideHeight"] = 0.21;
  synth_phys.values["FrontSpringDamper.MaxCompressHeight"] = 0.08;
  synth_phys.values["RearSpringDamper.DefRideHeight"] = 0.17;
  synth_phys.values["RearSpringDamper.MinRideHeight"] = 0.12;
  synth_phys.values["RearSpringDamper.MaxRideHeight"] = 0.22;
  synth_phys.values["RearSpringDamper.MaxCompressHeight"] = 0.08;
  synth_phys.values["List_UpgradeAntiSwayFront.Level"] = 3.0;
  synth_phys.values["List_UpgradeAntiSwayRear.Level"] = 3.0;
  synth_phys.values["List_UpgradeBrakes.Level"] = 3.0;
  synth_phys.values["List_UpgradeDrivetrainDifferential.Level"] = 3.0;
  synth_phys.values["Data_Drivetrain.DrivetypeID"] = 3.0;
  synth_phys.values["FrontBumperAero.Drag0"] = 5.0;
  synth_phys.values["FrontBumperAero.Drag1"] = 25.0;
  synth_phys.values["FrontBumperAero.Downforce0"] = 10.0;
  synth_phys.values["FrontBumperAero.Downforce1"] = 120.0;
  synth_phys.values["RearWingAero.Drag0"] = 8.0;
  synth_phys.values["RearWingAero.Drag1"] = 40.0;
  synth_phys.values["RearWingAero.Downforce0"] = 20.0;
  synth_phys.values["RearWingAero.Downforce1"] = 220.0;

  drive::CarTune tune;
  const auto sliders = build_tune_sliders(synth_phys, tune);
  CHECK(sliders.size() >= 30);
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_frontTirePressure", 28.5));
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_finalDriveRatio", 4.10));
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_frontCamber", -2.0));
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_frontRideHeight", 11.5));
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_rearDownforce", 200.0));
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_brakeBalance", 55.0));
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_brakePressure", 130.0));
  CHECK(set_tune_slider(tune, synth_phys, "Tuning_centerTorque", 75.0));
  CHECK(!tune.is_default());

  drive::CarParams tuned_p = base_p;
  tuned_p.layout = drive::CarParams::Layout::kAll;
  tuned_p.aero_drag_long = 0.25;
  const double orig_com_y = tuned_p.centre_of_mass.y;
  drive::apply_tune(tune, synth_phys, tuned_p);
  CHECK(std::fabs(tuned_p.final_drive - 4.10) < 1e-4);
  CHECK(tuned_p.centre_of_mass.y < orig_com_y);
  CHECK(tuned_p.aero_part_df_rear > 0.3);
  CHECK(std::fabs(tuned_p.brake_bias_slider - 0.55) < 1e-4);
  CHECK(std::fabs(tuned_p.rear_split - 0.75) < 1e-4);

  // 3b. Retail FH1 Tuning Level 3 (Race) unlock gating & unlock hints.
  {
    cars::CarPhysics sport_phys = synth_phys;
    sport_phys.values["List_UpgradeDrivetrainTransmission.Level"] = 2.0;  // Sport (must stay locked!)
    sport_phys.values["List_UpgradeSpringDamper.Level"] = 2.0;
    sport_phys.values["List_UpgradeAntiSwayFront.Level"] = 2.0;
    sport_phys.values["List_UpgradeAntiSwayRear.Level"] = 2.0;
    sport_phys.values["List_UpgradeBrakes.Level"] = 2.0;
    sport_phys.values["List_UpgradeDrivetrainDifferential.Level"] = 2.0;
    sport_phys.values["FrontBumperAero.Downforce1"] = 10.0;  // no aero range
    sport_phys.values["RearWingAero.Downforce1"] = 20.0;

    drive::CarTune locked_tune;
    const auto sport_sliders = build_tune_sliders(sport_phys, locked_tune);
    int tire_unlocked = 0, non_tire_locked = 0;
    for (const auto& sl : sport_sliders) {
      if (sl.category == TuneCategory::kTires) {
        CHECK(sl.unlocked);
        ++tire_unlocked;
      } else {
        CHECK(!sl.unlocked);
        CHECK(!sl.unlock_hint.empty());
        ++non_tire_locked;
      }
    }
    CHECK(tire_unlocked == 2 && non_tire_locked >= 20);
    // Attempting to modify a locked slider must fail and leave tune untouched.
    CHECK(!set_tune_slider(locked_tune, sport_phys, "Tuning_finalDriveRatio", 4.10));
    CHECK(!set_tune_slider(locked_tune, sport_phys, "Tuning_frontCamber", -2.0));
    CHECK(!set_tune_slider(locked_tune, sport_phys, "Tuning_brakeBalance", 60.0));
    CHECK(!set_tune_slider(locked_tune, sport_phys, "Tuning_centerTorque", 70.0));
    CHECK(locked_tune.is_default());
    // Tire pressure stays unlocked even on stock/sport parts.
    CHECK(set_tune_slider(locked_tune, sport_phys, "Tuning_frontTirePressure", 31.0));
  }

  // 3c. Monotonic headroom PI calculation (stock exact, heavy upgrades converge smoothly without drift).
  {
    cars::PhysicsSettings pset;
    cars::CarPhysics stk_phys = synth_phys;
    stk_phys.values["Data_Car.PerformanceIndex"] = 0.50;
    stk_phys.values["Data_Car.CurbWeight"] = 1450.0;
    stk_phys.values["List_TorqueCurve.TorqueScale"] = 250.0;
    const auto r_stock = compute_pi(stk_phys, pset, synth_cfg, bands, &stk_phys);
    CHECK(std::fabs(r_stock.raw_pi - 0.50) < 1e-6);

    cars::CarPhysics mid_phys = stk_phys;
    mid_phys.values["List_TorqueCurve.TorqueScale"] = 410.0;
    mid_phys.values["Data_Car.CurbWeight"] = 1320.0;
    const auto r_mid = compute_pi(mid_phys, pset, synth_cfg, bands, &stk_phys);

    cars::CarPhysics fast_phys = stk_phys;
    fast_phys.values["List_TorqueCurve.TorqueScale"] = 680.0;
    fast_phys.values["Data_Car.CurbWeight"] = 1150.0;
    const auto r_fast = compute_pi(fast_phys, pset, synth_cfg, bands, &stk_phys);
    CHECK(r_mid.raw_pi > r_stock.raw_pi && r_fast.raw_pi > r_mid.raw_pi && r_fast.raw_pi <= 1.0);
  }

  // garage.txt v2 round-trip with tune and livery.
  g.cars[0].tune = tune;
  g.cars[0].livery_file = "liveries/1.liv";
  Garage rt;
  CHECK(from_text(to_text(g), rt, &err));
  CHECK(rt.cars.size() == 1);
  CHECK(std::fabs(rt.cars[0].tune.front_tire_pressure_psi - 28.5) < 1e-4);
  CHECK(std::fabs(rt.cars[0].tune.final_drive - 4.10) < 1e-4);
  CHECK(std::fabs(rt.cars[0].tune.front_camber_deg - (-2.0)) < 1e-4);
  CHECK(std::fabs(rt.cars[0].tune.center_diff_split - 0.75) < 1e-4);
  CHECK(rt.cars[0].livery_file == "liveries/1.liv");

  // P0-7 synthetic player_pi propagation: upgraded garage car returns upgraded raw PI,
  // case-insensitive media matching succeeds, loaner override returns < 0, null returns < 0.
  rt.cars[0].performance_index = 0.725;
  std::map<int64_t, std::string> baked_ids{{rt.cars[0].car_id, "VW_CorradoVR6_95"}};
  CHECK(std::fabs(race::resolve_player_race_pi(&rt.cars[0], "vw_corradovr6_95", baked_ids, nullptr, nullptr, nullptr,
                                               nullptr) -
                  0.725) < 1e-6);
  CHECK(race::resolve_player_race_pi(&rt.cars[0], "FOR_FocusRS_09", baked_ids, nullptr, nullptr, nullptr, nullptr) <
        0.0);
  CHECK(race::resolve_player_race_pi(nullptr, "VW_CorradoVR6_95", baked_ids, nullptr, nullptr, nullptr, nullptr) < 0.0);
}

static void test_disc_pi_conversions_and_auto_upgrade() {
  using namespace garage;
  const fs::path game_root = R"(../horizon1-recomp/.local\game\base)";
  if (!fs::exists(game_root / "media" / "physics.zip")) {
    std::printf("pi_tuning_tests: skipping disc oracle checks (no game root)\n");
    return;
  }

  cars::Install install;
  std::string err;
  CHECK(cars::discover_install(game_root, "", "", install, &err));
  cars::PartsDb db;
  CHECK(db.open(install, &err));
  cars::PhysicsSettings phys_settings;
  CHECK(cars::load_physics_settings(install.media / "physics.zip", phys_settings, &err));
  PiConfig pi_cfg;
  CHECK(load_pi_config(install.media / "physics.zip", pi_cfg, &err));
  CHECK(pi_cfg.segments.size() == 15);
  std::vector<ClassBand> bands;
  CHECK(load_class_bands(game_root, bands, &err));
  CHECK(bands.size() == 11);
  // Merged DB overload must return the same 11 class bands including Class X (999).
  std::vector<ClassBand> db_bands;
  CHECK(load_class_bands(db, db_bands, &err));
  CHECK(db_bands.size() == 11 && db_bands.back().name == "X" && int(db_bands.back().shown_max) == 999);

  // Stock cars must match Data_Car.PerformanceIndex exactly.
  for (auto [car_id, exp_cls, exp_shown] : {std::tuple<int64_t, int, int>{309, 2, 282},
                                            std::tuple<int64_t, int, int>{445, 4, 461},
                                            std::tuple<int64_t, int, int>{1009, 4, 437}}) {
    PiResult res;
    CHECK(compute_pi_for_car(db, car_id, {}, phys_settings, pi_cfg, bands, res, &err));
    std::printf("  stock car %lld: raw=%.5f (lap_raw=%.5f T=%.2fs) -> %s %d (expected cls=%d shown=%d)\n",
                (long long)car_id, res.raw_pi, res.raw_lap_pi, res.lap_time_s,
                bands[size_t(res.class_id)].name.c_str(), res.shown_pi, exp_cls, exp_shown);
    CHECK(res.class_id == exp_cls);
    CHECK(std::abs(res.shown_pi - exp_shown) <= 1);
  }

  // Engine conversion + Aspiration conversion on Corrado (309).
  {
    Garage g;
    std::map<std::string, int64_t> stock;
    CHECK(db.stock_slots(309, stock, &err));
    int64_t credits = 500000;
    int64_t gid = 0;
    CHECK(buy_car(g, 309, 10000, 0.525, 2, 1, stock, credits, &gid));
    PiResult pi0;
    CHECK(refresh_car_pi(*g.find(gid), db, phys_settings, pi_cfg, bands, &pi0));

    // Find an engine swap (part_enum == 0, non-stock).
    std::vector<cars::PartRow> opts;
    CHECK(db.options(309, fitted(*g.find(gid)), opts, &err));
    const cars::PartRow* swap = nullptr;
    for (const auto& o : opts)
      if (o.part_enum == 0 && !o.is_stock) { swap = &o; break; }
    CHECK(swap != nullptr);
    if (swap) {
      PiResult pi_swap;
      FitResult fr = fit_part_full(g, db, gid, *swap, "Engine", 0, credits, &phys_settings, &pi_cfg, &bands, &pi_swap);
      CHECK(fr.ok && fr.rekeyed);
      std::printf("  Corrado engine swap: %s %d -> %s %d (P %.5f -> %.5f)\n",
                  bands[size_t(pi0.class_id)].name.c_str(), pi0.shown_pi,
                  bands[size_t(pi_swap.class_id)].name.c_str(), pi_swap.shown_pi, pi0.raw_pi, pi_swap.raw_pi);
      CHECK(pi_swap.raw_pi > pi0.raw_pi);
    }

    // Auto Upgrade Corrado from D to A class (class_id = 5, ceiling 0.739 = A 600).
    Garage g2;
    int64_t gid2 = 0;
    credits = 500000;
    CHECK(buy_car(g2, 309, 10000, pi0.raw_pi, pi0.class_id, 1, stock, credits, &gid2));
    auto plan = plan_auto_upgrade(g2, db, gid2, 5, 0, phys_settings, pi_cfg, bands);
    CHECK(plan.ok);
    std::printf("  Corrado Auto Upgrade D -> A: %s %d (%zu steps, %lld CR)\n",
                bands[size_t(plan.result.class_id)].name.c_str(), plan.result.shown_pi, plan.steps.size(),
                (long long)plan.total_cost);
    CHECK(plan.result.class_id == 5 && plan.result.shown_pi >= 580 && plan.result.shown_pi <= 600);
    std::string msg;
    CHECK(apply_auto_upgrade(g2, db, gid2, plan, 0, credits, phys_settings, pi_cfg, bands, &msg));
    CHECK(g2.find(gid2)->class_id == 5);

    // P0-7: resolve_player_race_pi must return upgraded raw PI for the fitted garage car,
    // not stock PI, and return < 0 for a non-matching loaner/override car.
    std::map<int64_t, std::string> baked_ids{{309, "VW_CorradoVR6_95"}};
    const double upgraded_pi = race::resolve_player_race_pi(
        g2.find(gid2), "VW_CorradoVR6_95", baked_ids, &db, &phys_settings, &pi_cfg, &bands);
    CHECK(upgraded_pi > pi0.raw_pi + 0.10);
    CHECK(std::fabs(upgraded_pi - plan.result.raw_pi) < 1e-4);
    const double loaner_pi = race::resolve_player_race_pi(
        g2.find(gid2), "NIS_SkylineGTR_02", baked_ids, &db, &phys_settings, &pi_cfg, &bands);
    CHECK(loaner_pi < 0.0);

    // Auto Downgrade Corrado back down to C class (class_id = 3, ceiling 0.5975 = C 400).
    auto down = plan_auto_upgrade(g2, db, gid2, 3, 0, phys_settings, pi_cfg, bands);
    CHECK(down.ok && down.result.class_id == 3 && down.result.shown_pi <= 400);
    CHECK(down.result.raw_pi <= bands[3].raw_max);
    std::printf("  Corrado Auto Downgrade A -> C: %s %d (%zu steps)\n",
                bands[size_t(down.result.class_id)].name.c_str(), down.result.shown_pi, down.steps.size());

    // Auto Upgrade strict ceiling (zero spillover into next class) & Class X (10) support.
    for (int target_cls : {3, 4, 5, 6, 10}) {
      auto pcls = plan_auto_upgrade(g2, db, gid2, target_cls, 0, phys_settings, pi_cfg, bands);
      CHECK(pcls.ok);
      CHECK(pcls.result.class_id <= target_cls);
      CHECK(pcls.result.raw_pi <= bands[size_t(target_cls)].raw_max);
    }
    // Mechanical target (C class) should not unnecessarily force visual aero bumpers/wings.
    auto p_c_from_stock = plan_auto_upgrade(g, db, gid, 3, 0, phys_settings, pi_cfg, bands);
    if (p_c_from_stock.ok) {
      CHECK(p_c_from_stock.result.class_id <= 3 && p_c_from_stock.result.raw_pi <= bands[3].raw_max);
    }
  }

  // Oracle check against user-modded PlayerDatabase.sqlite upgraded cars (if present).
  const fs::path oracle_db = R"(out\profile\user-modded\PlayerDatabase.sqlite)";
  if (fs::exists(oracle_db)) {
    sqlite3* odb = nullptr;
    if (sqlite3_open_v2(oracle_db.string().c_str(), &odb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
      std::printf("  --- Oracle PI check on user-modded upgraded cars ---\n");
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(odb, "SELECT * FROM Career_Garage ORDER BY Id", -1, &st, nullptr) == SQLITE_OK) {
        int checked = 0, within2 = 0, within5 = 0;
        double sum_abs_shown = 0.0;
        while (sqlite3_step(st) == SQLITE_ROW) {
          int64_t gid = 0, car_id = 0;
          double oracle_pi = 0.0;
          int oracle_cls = 0;
          std::map<std::string, int64_t> slots;
          for (int c = 0; c < sqlite3_column_count(st); ++c) {
            const std::string col = sqlite3_column_name(st, c);
            if (col == "Id") gid = sqlite3_column_int64(st, c);
            else if (col == "CarId") car_id = sqlite3_column_int64(st, c);
            else if (col == "PerformanceIndex") oracle_pi = sqlite3_column_double(st, c);
            else if (col == "ClassID") oracle_cls = sqlite3_column_int(st, c);
            else if (sqlite3_column_type(st, c) == SQLITE_INTEGER) slots[col] = sqlite3_column_int64(st, c);
          }
          Car car;
          car.id = gid;
          car.car_id = car_id;
          car.slots = slots;
          PiResult res;
          if (!compute_pi_for_car(db, car_id, fitted(car), phys_settings, pi_cfg, bands, res, nullptr)) continue;
          const int o_shown = shown_index(oracle_pi, bands);
          const int diff = res.shown_pi - o_shown;
          ++checked;
          sum_abs_shown += std::abs(diff);
          if (std::abs(diff) <= 2) ++within2;
          if (std::abs(diff) <= 5) ++within5;
          std::printf("    gid=%2lld car=%4lld: oracle=%s%3d (%.4f) vs calc=%s%3d (%.4f, raw_lap=%.4f) diff=%+d\n",
                      (long long)gid, (long long)car_id, bands[size_t(oracle_cls)].name.c_str(), o_shown, oracle_pi,
                      bands[size_t(res.class_id)].name.c_str(), res.shown_pi, res.raw_pi, res.raw_lap_pi, diff);
        }
        sqlite3_finalize(st);
        if (checked > 0) {
          std::printf("  Oracle summary: %d cars, mean |diff|=%.2f, within +/-2: %d/%d, within +/-5: %d/%d\n",
                      checked, sum_abs_shown / checked, within2, checked, within5, checked);
        }
      }
      sqlite3_close(odb);
    }
  }
}

int main() {
  test_synthetic_pi_and_tuning();
  test_disc_pi_conversions_and_auto_upgrade();
  if (failures == 0) std::printf("pi_tuning_tests: all passed\n");
  return failures ? 1 : 0;
}
