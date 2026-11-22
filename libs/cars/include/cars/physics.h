// A stock car's physics rows from the merged game database, as raw columns,
// and the game-wide PhysicsSettings.ini tunables. The drive model turns
// them into its own parameters (libs/drive, from_physics.h); this side only
// finds and carries the rows.
// Spec: SPEC-CAR-PHYSICS (Car parameters: how a stock car's rows are found;
// Global settings); FORMAT-CARS (DLC packages: database rows).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cars/sources.h"

struct sqlite3_stmt;

namespace cars {

// Values are keyed "<group>.<column>", the group naming the row:
//   Data_Car, Data_CarBody, Data_Engine, Data_Drivetrain, and each stock
//   upgrade row by its table (List_UpgradeEngineCamshaft, ...);
//   List_TorqueCurve (the camshaft's full-throttle curve, scalars);
//   FrontSpringDamper, RearSpringDamper, FrontAntiSway, RearAntiSway;
//   List_TireCompound, LatMultiCurve, LongMultiCurve.
// Curves: "torque" (List_TorqueCurve v0..), "lat0", "lat1", "long0",
// "long1" (List_TireFrictionCurve v0.. of the multi curves' ID0 and ID1).
// Only numeric columns are kept. A stock row that does not exist (a car
// without a turbo) leaves its group out.
struct CarPhysics {
  int64_t car_id = 0;
  std::string media_name;
  std::map<std::string, double> values;
  std::map<std::string, std::vector<double>> curves;

  bool has(const std::string& key) const { return values.count(key) != 0; }
  double get(const std::string& key, double fallback = 0) const {
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
  const std::vector<double>* curve(const std::string& name) const {
    auto it = curves.find(name);
    return it == curves.end() ? nullptr : &it->second;
  }
};

// PhysicsSettings.ini: "Section\Key value" lines; numeric values only,
// keyed as written ("SidewallScalePeakSA\TireSidewall0").
struct PhysicsSettings {
  std::map<std::string, double> values;
  double get(const std::string& key, double fallback) const {
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
};

bool parse_physics_settings(std::string_view text, PhysicsSettings& out, std::string* error);
// media/physics.zip member PhysicsSettings.ini.
bool load_physics_settings(const std::filesystem::path& physics_zip, PhysicsSettings& out, std::string* error);

// The fitted parts of a car (SPEC-GARAGE-UPGRADES, Areas, slots and
// tables): the List_Upgrade* table name to the fitted row's Id. A table
// not named (or -1) keeps its stock row.
using FittedParts = std::map<std::string, int64_t>;

// The stock rows of each car in `ids` (Data_Car.Id) over the merged
// database (load_database's merge). A car whose links do not resolve is an
// error naming the link. With `fitted` (one per id), each named table's
// fitted row replaces its stock row (the sub-part tables then follow the
// fitted engine, drivetrain and body keys), the fitted rows of the tables
// the stock chain does not read are loaded too, and the delta columns
// combine (SPEC-GARAGE-UPGRADES, What a part changes): the mass is the
// weight row's Mass plus every other fitted row's MassDiff; the torque
// scale times each fitted engine part's TorqueScale; the front share plus
// every WeightDistDiff; the CoM height plus the chassis row's CMHeightDiff;
// the tyre widths and rim diameters from the fitted width and rim rows.
bool load_car_physics(const Install& install, const std::vector<int64_t>& ids, std::vector<CarPhysics>& out,
                      std::string* error, const std::vector<FittedParts>* fitted = nullptr);

// An upgrade slot (SPEC-GARAGE-UPGRADES, Areas, slots and tables): its
// part enum, its Career_Garage column, its table and the table's key
// (0 the car's Ordinal, 1 EngineID, 2 DrivetrainID, 3 CarBodyID).
struct UpgradeSlot {
  int part_enum;
  const char* column;
  const char* table;
  int key;
};
const std::vector<UpgradeSlot>& upgrade_slots();

// A row a slot offers.
struct PartRow {
  int part_enum = 0;
  std::string table;
  int64_t id = 0;
  int level = 0;
  bool is_stock = false;
  int64_t price = 0, manufacturer = 0;
};

// A car the Autoshow can list (SPEC-AUTOSHOW-PAINT, Which cars are
// listed: drivable, selectable, not a unicorn, with a PowerTrains row).
struct DealerCar {
  int64_t id = 0, base_cost = 0, year = 0, class_id = 0;
  double performance_index = 0;
  std::string media_name;
  int64_t first_colour = 0;  // the lowest Combo_Colors Sequence: the swatch the showroom opens on
  bool is_dlc = false;       // mapped by a DLC ContentOffersMapping row
};

// The merged database kept open (opening merges every package, so the
// garage opens it once): a car's rows with its fitted parts, and the rows
// each slot offers for the car (keyed through its fitted engine,
// drivetrain and body).
class PartsDb {
 public:
  PartsDb();
  ~PartsDb();
  PartsDb(const PartsDb&) = delete;
  PartsDb& operator=(const PartsDb&) = delete;
  bool open(const Install& install, std::string* error);
  bool is_open() const { return impl_ != nullptr; }
  bool physics(int64_t car_id, const FittedParts& fit, CarPhysics& out, std::string* error) const;
  bool options(int64_t car_id, const FittedParts& fit, std::vector<PartRow>& out, std::string* error) const;
  // The listable cars, by ClassID, PerformanceIndex, then Id.
  // When `include_hidden` is true, also includes the 4 qualified hidden variants
  // (1565 Intro Corrado, 1566 Showcase Mini, 1567 500 Abarth, 1568 205 T16).
  bool dealer(std::vector<DealerCar>& out, std::string* error, bool include_hidden = false) const;
  // A new car's slots: each slot's stock row (the first by Id when a table
  // has several), by Career_Garage column.
  bool stock_slots(int64_t car_id, std::map<std::string, int64_t>& out, std::string* error) const;
  // Runs `sql` with text parameters over the merged database, calling `row`
  // per result row under the database lock.
  bool each(const std::string& sql, const std::vector<std::string>& params,
            const std::function<void(sqlite3_stmt*)>& row, std::string* error) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// SPEC-CAMERAS' per-car inputs: the car's CameraOverrides row (by CarId;
// every column, zero when the car has no row) and its collision box,
// MAXData.xml Collision/BoundingBox from the car archive (MinX MinY MinZ
// MaxX MaxY MaxZ as written: x and y in the car frame, z before the
// BottomCenterWheelbasePosZ shift).
struct CarCameraRow {
  std::map<std::string, double> overrides;  // column name -> value
  bool has_overrides = false;
  std::array<double, 6> box{};
  bool has_box = false;
};
bool load_car_camera(const Install& install, int64_t car_id, const std::string& media, CarCameraRow& out,
                     std::string* error);
// media/camera.zip's CameraSettings.ini and CameraPhysics.xml (or
// CameraPhysicsSansEffects.xml when camera_effects is false); v4's own
// camera/CameraPhysics.xml from its media.zip when there is one (SPEC-
// CAMERAS, Inputs).
bool load_camera_files(const Install& install, std::string& settings_ini, std::string& physics_xml,
                       std::string* error, bool camera_effects = true);
// media/ControllerRumble.xml, or v4's media.zip controllerrumble.xml when
// installed (SPEC-CAMERAS, Gamepad rumble).
bool load_rumble_xml(const Install& install, std::string& xml, std::string* error);

// A text form for the bake: "value <key> <number>", "curve <name> <n>
// <numbers...>", "setting <key> <number>" and "car <id> <media name>".
std::string physics_to_text(const CarPhysics& car, const PhysicsSettings& settings);
bool physics_from_text(std::string_view text, CarPhysics& car, PhysicsSettings& settings, std::string* error);

}  // namespace cars
